// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_TUI_APP_HPP
#define TRANSPORTER_TUI_APP_HPP

namespace transporter::engine {
class Engine;
} // namespace transporter::engine

namespace transporter::library {
class Library;
} // namespace transporter::library

namespace transporter::tui {

enum class PageId {
    Library    = 1,
    Queue      = 2,
    NowPlaying = 3,
    Pipeline   = 4,
    Settings   = 5,
};

// Run the TUI main loop. notcurses must already be initialised via init().
// engine and library may be null on early-error paths; callers gate access.
// Returns 0 on clean exit, non-zero on fatal error.
int run(engine::Engine* engine, library::Library* library);

} // namespace transporter::tui

#endif
