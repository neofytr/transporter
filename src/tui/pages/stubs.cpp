// SPDX-License-Identifier: GPL-3.0-or-later

#include "pages.hpp"

#include "../app.hpp"

#include <notcurses/notcurses.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <string_view>

namespace transporter::tui::pages {

namespace {

// Draw a rounded-border rectangle into `host` covering body region. The
// title is embedded into the top edge as "╭─ Title ────...─╮".
void draw_frame(struct ncplane* host, int y0, int rows,
                std::string_view title) {
    unsigned hrows = 0, hcols = 0;
    ncplane_dim_yx(host, &hrows, &hcols);
    if (rows < 3 || hcols < 6) {
        return;
    }
    const int w = static_cast<int>(hcols);
    const int x_right = w - 1;
    const int y_bottom = y0 + rows - 1;

    // Top edge with embedded title.
    ncplane_putstr_yx(host, y0, 0, "╭");
    ncplane_putstr_yx(host, y0, 1, "─ ");
    ncplane_putstr_yx(host, y0, 3, title.data());
    const int title_end = 3 + static_cast<int>(title.size());
    ncplane_putstr_yx(host, y0, title_end, " ");
    for (int x = title_end + 1; x < x_right; ++x) {
        ncplane_putstr_yx(host, y0, x, "─");
    }
    ncplane_putstr_yx(host, y0, x_right, "╮");

    // Side edges.
    for (int y = y0 + 1; y < y_bottom; ++y) {
        ncplane_putstr_yx(host, y, 0, "│");
        ncplane_putstr_yx(host, y, x_right, "│");
    }

    // Bottom edge.
    ncplane_putstr_yx(host, y_bottom, 0, "╰");
    for (int x = 1; x < x_right; ++x) {
        ncplane_putstr_yx(host, y_bottom, x, "─");
    }
    ncplane_putstr_yx(host, y_bottom, x_right, "╯");
}

// Put a centered string at row y within the host plane.
void put_centered(struct ncplane* host, int y, std::string_view s) {
    unsigned rows = 0, cols = 0;
    ncplane_dim_yx(host, &rows, &cols);
    (void)rows;
    const int x = (static_cast<int>(cols) - static_cast<int>(s.size())) / 2;
    if (x < 0) {
        return;
    }
    ncplane_putstr_yx(host, y, x, s.data());
}

void draw_stub(struct ncplane* host, int y0, int rows,
               std::string_view title,
               std::string_view big_label,
               std::string_view subtitle) {
    draw_frame(host, y0, rows, title);
    if (rows < 4) {
        return;
    }
    const int mid = y0 + rows / 2;
    ncplane_set_styles(host, NCSTYLE_BOLD);
    put_centered(host, mid - 1, big_label);
    ncplane_set_styles(host, NCSTYLE_NONE);
    ncplane_set_fg_rgb8(host, 0x90, 0x90, 0x90);
    put_centered(host, mid + 1, subtitle);
    ncplane_set_fg_default(host);
}

} // namespace

void draw_library(struct ncplane* host, int body_y0, int body_rows) {
    draw_stub(host, body_y0, body_rows,
              "Library",
              "Library",
              "Browse your albums — coming in T2");
}

void draw_queue(struct ncplane* host, int body_y0, int body_rows) {
    draw_stub(host, body_y0, body_rows,
              "Queue",
              "Queue",
              "Up next — coming in T4");
}

void draw_now_playing(struct ncplane* host, int body_y0, int body_rows) {
    draw_stub(host, body_y0, body_rows,
              "Now Playing",
              "Now Playing",
              "Cover · spectrum · VU — coming in T3 / T6");
}

void draw_pipeline(struct ncplane* host, int body_y0, int body_rows) {
    draw_stub(host, body_y0, body_rows,
              "Pipeline",
              "Pipeline",
              "Dense per-stage telemetry — coming in T5");
}

void draw_settings(struct ncplane* host, int body_y0, int body_rows) {
    draw_stub(host, body_y0, body_rows,
              "Settings",
              "Settings",
              "Tabbed runtime knobs — coming in T8");
}

std::string_view hint_for(int page_id) {
    switch (page_id) {
    case 1: return "j/k navigate · Enter play · / search · D dac · ? help";
    case 2: return "J/K reorder · dd remove · D clear · ? help";
    case 3: return "space play/pause · h/l seek · n next · ? help";
    case 4: return "? help";
    case 5: return "Tab tabs · ? help";
    default: return "";
    }
}

} // namespace transporter::tui::pages
