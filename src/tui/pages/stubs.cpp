// SPDX-License-Identifier: GPL-3.0-or-later

#include "pages.hpp"

#include "../app.hpp"
#include "draw_util.hpp"

#include <notcurses/notcurses.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <string_view>

namespace transporter::tui::pages {

namespace {

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
