// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.hpp"

#include "components/player_bar.hpp"
#include "notcurses_ctx.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/engine/telemetry.hpp>

#include <notcurses/notcurses.h>

#include <cstdint>
#include <string_view>

namespace transporter::tui {

namespace {

void draw_body_placeholder(struct ncplane* p, int body_rows, int body_cols) {
    // Centered "transporter" label while the page router is not yet wired.
    constexpr std::string_view label = "transporter";
    const int y = body_rows / 2;
    const int x = (body_cols - static_cast<int>(label.size())) / 2;
    if (x >= 0 && y >= 0) {
        ncplane_putstr_yx(p, y, x, label.data());
    }
}

void render_frame(struct notcurses* nc, engine::Engine* engine) {
    struct ncplane* std_plane = notcurses_stdplane(nc);
    unsigned rows = 0, cols = 0;
    ncplane_dim_yx(std_plane, &rows, &cols);
    ncplane_erase(std_plane);

    const int body_rows = static_cast<int>(rows)
                        - components::kPlayerBarRows;
    if (body_rows > 0) {
        draw_body_placeholder(std_plane, body_rows, static_cast<int>(cols));
    }

    if (engine != nullptr) {
        const auto snap = engine->pipeline_snapshot();
        const bool playing = engine->state() == engine::State::Playing;
        components::draw_player_bar(std_plane, &snap, /*volume_pct=*/65,
                                    playing);
    } else {
        components::draw_player_bar(std_plane, nullptr, 0, false);
    }

    notcurses_render(nc);
}

} // namespace

int run(engine::Engine* engine, library::Library* /*library*/) {
    struct notcurses* nc = current_context();
    if (nc == nullptr) {
        return -1;
    }

    render_frame(nc, engine);

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
            render_frame(nc, engine);
            continue;
        }
        if (r == 'q' || r == 'Q') {
            break;
        }
        if (r == 'c' && (ni.modifiers & NCKEY_MOD_CTRL) != 0) {
            break;
        }
        render_frame(nc, engine);
    }
    return 0;
}

} // namespace transporter::tui
