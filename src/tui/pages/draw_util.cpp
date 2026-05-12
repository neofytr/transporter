// SPDX-License-Identifier: GPL-3.0-or-later

#include "draw_util.hpp"

#include <notcurses/notcurses.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace transporter::tui::pages {

void draw_frame(struct ncplane* host, int y0, int rows,
                std::string_view title) {
    unsigned hrows = 0, hcols = 0;
    ncplane_dim_yx(host, &hrows, &hcols);
    (void)hrows;
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

std::string clip_with_ellipsis(std::string_view s, std::size_t max_cells) {
    if (s.size() <= max_cells) {
        return std::string(s);
    }
    if (max_cells == 0) {
        return std::string{};
    }
    // Reserve room for the ellipsis (counted as one cell of display width).
    std::size_t budget = max_cells > 1 ? max_cells - 1 : 0;
    while (budget > 0
           && (static_cast<unsigned char>(s[budget]) & 0xC0) == 0x80) {
        --budget;
    }
    std::string out(s.substr(0, budget));
    out.append("…");
    return out;
}

} // namespace transporter::tui::pages
