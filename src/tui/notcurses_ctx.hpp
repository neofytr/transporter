// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_TUI_NOTCURSES_CTX_HPP
#define TRANSPORTER_TUI_NOTCURSES_CTX_HPP

#include <string>

struct notcurses;

namespace transporter::tui {

struct Capabilities {
    bool kitty_graphics = false;
    bool sixel          = false;
    bool truecolor      = false;
    bool unicode        = true;   // notcurses minimum
    bool inside_tmux    = false;
    bool inside_screen  = false;
    std::string term;             // $TERM
    std::string term_program;     // $TERM_PROGRAM if set
    int cells_cols  = 0;
    int cells_rows  = 0;
    int pixel_cols  = 0;          // if reported by terminal
    int pixel_rows  = 0;
};

// Probe terminal capabilities WITHOUT taking over the terminal. Safe to call
// before/instead of init(). Used by --probe-terminal. Always returns; on probe
// failure the returned struct holds only the env-derived fields.
Capabilities probe_capabilities();

struct InitOptions {
    bool enable_mouse = true;
};

// Initialise notcurses, take over the terminal. Returns 0 on success.
int init(const InitOptions& opts = {});

// Tear down notcurses cleanly. Idempotent.
void shutdown();

// Return the live notcurses context, or nullptr if not yet initialised.
struct notcurses* current_context();

// Print a freshly-probed capability matrix to stdout, one line per field.
void print_capabilities();

} // namespace transporter::tui

#endif
