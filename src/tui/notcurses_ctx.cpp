// SPDX-License-Identifier: GPL-3.0-or-later

#include "notcurses_ctx.hpp"

#include <notcurses/notcurses.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace transporter::tui {

namespace {

std::atomic<struct notcurses*> g_nc{nullptr};
Capabilities g_caps{};
std::atomic<bool> g_caps_ready{false};

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return (v != nullptr) ? std::string{v} : std::string{};
}

// Build the options used by the silent probe pass.
notcurses_options probe_options() {
    notcurses_options o{};
    o.flags = NCOPTION_NO_ALTERNATE_SCREEN
            | NCOPTION_SUPPRESS_BANNERS
            | NCOPTION_PRESERVE_CURSOR
            | NCOPTION_DRAIN_INPUT;
    o.loglevel = NCLOGLEVEL_SILENT;
    return o;
}

// Options for the live, fullscreen TUI session.
notcurses_options run_options() {
    notcurses_options o{};
    o.flags    = NCOPTION_SUPPRESS_BANNERS;
    o.loglevel = NCLOGLEVEL_SILENT;
    return o;
}

void fill_env(Capabilities& c) {
    c.term         = env_or_empty("TERM");
    c.term_program = env_or_empty("TERM_PROGRAM");
    c.inside_tmux  = !env_or_empty("TMUX").empty();
    c.inside_screen = c.term.rfind("screen", 0) == 0
                   || !env_or_empty("STY").empty();
}

// Fill `c` from the live notcurses instance `nc`. Cheap — only queries
// already-cached state inside notcurses.
void fill_from_instance(struct notcurses* nc, Capabilities& c) {
    fill_env(c);
    c.truecolor      = notcurses_cantruecolor(nc);
    c.unicode        = notcurses_canutf8(nc);
    const ncpixelimpl_e pix = notcurses_check_pixel_support(nc);
    c.sixel          = (pix == NCPIXEL_SIXEL);
    c.kitty_graphics = (pix == NCPIXEL_KITTY_STATIC
                     || pix == NCPIXEL_KITTY_ANIMATED
                     || pix == NCPIXEL_KITTY_SELFREF);

    unsigned rows = 0, cols = 0;
    notcurses_term_dim_yx(nc, &rows, &cols);
    c.cells_rows = static_cast<int>(rows);
    c.cells_cols = static_cast<int>(cols);

    unsigned pxy = 0, pxx = 0;
    ncplane_pixel_geom(notcurses_stdplane(nc),
                       &pxy, &pxx, nullptr, nullptr, nullptr, nullptr);
    c.pixel_rows = static_cast<int>(pxy);
    c.pixel_cols = static_cast<int>(pxx);
}

} // namespace

Capabilities probe_capabilities() {
    Capabilities c;
    fill_env(c);

    // Probing requires a notcurses context. Use /dev/tty so the probe works
    // even when stdout is redirected; if /dev/tty cannot be opened (no
    // controlling tty), fall back to env-only results.
    FILE* tty = std::fopen("/dev/tty", "r+e");
    if (tty == nullptr) {
        return c;
    }

    notcurses_options opts = probe_options();
    struct notcurses* nc = notcurses_core_init(&opts, tty);
    if (nc == nullptr) {
        std::fclose(tty);
        return c;
    }

    c.truecolor      = notcurses_cantruecolor(nc);
    c.unicode        = notcurses_canutf8(nc);
    const ncpixelimpl_e pix = notcurses_check_pixel_support(nc);
    c.sixel          = (pix == NCPIXEL_SIXEL);
    c.kitty_graphics = (pix == NCPIXEL_KITTY_STATIC
                     || pix == NCPIXEL_KITTY_ANIMATED
                     || pix == NCPIXEL_KITTY_SELFREF);

    unsigned rows = 0, cols = 0;
    notcurses_term_dim_yx(nc, &rows, &cols);
    c.cells_rows = static_cast<int>(rows);
    c.cells_cols = static_cast<int>(cols);

    // Pixel-geometry: notcurses exposes this via ncplane_pixel_geom on the
    // standard plane. Not all terminals report it; zero means unknown.
    unsigned pxy = 0, pxx = 0;
    ncplane_pixel_geom(notcurses_stdplane(nc),
                       &pxy, &pxx, nullptr, nullptr, nullptr, nullptr);
    c.pixel_rows = static_cast<int>(pxy);
    c.pixel_cols = static_cast<int>(pxx);

    notcurses_stop(nc);
    std::fclose(tty);
    return c;
}

int init(const InitOptions& iopts) {
    if (g_nc.load(std::memory_order_acquire) != nullptr) {
        return 0;
    }
    notcurses_options opts = run_options();
    struct notcurses* nc = notcurses_core_init(&opts, stdout);
    if (nc == nullptr) {
        return -1;
    }
    if (iopts.enable_mouse) {
        notcurses_mice_enable(nc, NCMICE_BUTTON_EVENT | NCMICE_DRAG_EVENT);
    }
    // Snapshot capabilities from the live instance. This is the authoritative
    // source for blitter dispatch — running probe_capabilities() in parallel
    // while this instance owns the tty races with terminal queries.
    Capabilities live;
    fill_from_instance(nc, live);
    g_caps = live;
    g_caps_ready.store(true, std::memory_order_release);
    g_nc.store(nc, std::memory_order_release);
    return 0;
}

void shutdown() {
    struct notcurses* nc = g_nc.exchange(nullptr, std::memory_order_acq_rel);
    if (nc != nullptr) {
        notcurses_stop(nc);
    }
    g_caps_ready.store(false, std::memory_order_release);
}

struct notcurses* current_context() {
    return g_nc.load(std::memory_order_acquire);
}

Capabilities cached_capabilities() {
    if (g_caps_ready.load(std::memory_order_acquire)) {
        return g_caps;
    }
    return probe_capabilities();
}

namespace {

const char* bool_str(bool b) { return b ? "yes" : "no"; }

} // namespace

void print_capabilities() {
    const Capabilities c = probe_capabilities();
    std::printf("term            %s\n", c.term.c_str());
    std::printf("term_program    %s\n", c.term_program.c_str());
    std::printf("inside_tmux     %s\n", bool_str(c.inside_tmux));
    std::printf("inside_screen   %s\n", bool_str(c.inside_screen));
    std::printf("kitty_graphics  %s\n", bool_str(c.kitty_graphics));
    std::printf("sixel           %s\n", bool_str(c.sixel));
    std::printf("truecolor       %s\n", bool_str(c.truecolor));
    std::printf("unicode         %s\n", bool_str(c.unicode));
    std::printf("cells           %d x %d\n", c.cells_cols, c.cells_rows);
    std::printf("pixels          %d x %d\n", c.pixel_cols, c.pixel_rows);
}

} // namespace transporter::tui
