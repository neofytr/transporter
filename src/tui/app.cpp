// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.hpp"

#include "notcurses_ctx.hpp"

#include <notcurses/notcurses.h>

#include <cstdint>
#include <string_view>

namespace transporter::tui {

namespace {

void draw_centered_label(struct notcurses* nc) {
    struct ncplane* std_plane = notcurses_stdplane(nc);
    unsigned rows = 0, cols = 0;
    ncplane_dim_yx(std_plane, &rows, &cols);
    ncplane_erase(std_plane);

    constexpr std::string_view label = "transporter";
    const int y = static_cast<int>(rows) / 2;
    const int x = (static_cast<int>(cols) - static_cast<int>(label.size())) / 2;
    if (x >= 0 && y >= 0) {
        ncplane_putstr_yx(std_plane, y, x, label.data());
    }
    notcurses_render(nc);
}

} // namespace

int run(engine::Engine* /*engine*/, library::Library* /*library*/) {
    struct notcurses* nc = current_context();
    if (nc == nullptr) {
        return -1;
    }

    draw_centered_label(nc);

    for (;;) {
        ncinput ni{};
        const uint32_t r = notcurses_get_blocking(nc, &ni);
        if (r == static_cast<uint32_t>(-1)) {
            break;
        }
        if (ni.evtype == NCTYPE_RELEASE) {
            continue;
        }
        if (r == NCKEY_RESIZE) {
            notcurses_refresh(nc, nullptr, nullptr);
            draw_centered_label(nc);
            continue;
        }
        if (r == 'q' || r == 'Q') {
            break;
        }
        if (r == 'c' && (ni.modifiers & NCKEY_MOD_CTRL) != 0) {
            break;
        }
    }
    return 0;
}

} // namespace transporter::tui
