// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.hpp"

#include "components/player_bar.hpp"
#include "input.hpp"
#include "notcurses_ctx.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/engine/telemetry.hpp>

#include <notcurses/notcurses.h>

#include <cstdint>
#include <string_view>

namespace transporter::tui {

namespace {

void draw_body_placeholder(struct ncplane* p, int body_rows, int body_cols) {
    constexpr std::string_view label = "transporter";
    const int y = body_rows / 2;
    const int x = (body_cols - static_cast<int>(label.size())) / 2;
    if (x >= 0 && y >= 0) {
        ncplane_putstr_yx(p, y, x, label.data());
    }
}

void render_frame(struct notcurses* nc, engine::Engine* engine,
                  const InputMap& input) {
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

    // Transient input bar at the very bottom row (overlays the player bar's
    // top edge — that's intentional; the modal nature is brief).
    if (input.mode() != Mode::Normal) {
        const char prefix = (input.mode() == Mode::Command) ? ':' : '/';
        const int y = static_cast<int>(rows) - 1;
        ncplane_set_fg_default(std_plane);
        for (int x = 0; x < static_cast<int>(cols); ++x) {
            ncplane_putchar_yx(std_plane, y, x, ' ');
        }
        ncplane_putchar_yx(std_plane, y, 0, prefix);
        ncplane_putstr_yx(std_plane, y, 1, input.buffer().c_str());
    }

    notcurses_render(nc);
}

bool dispatch_command(const CommandResult& cr, engine::Engine* engine,
                      InputMap& input) {
    switch (cr.cmd) {
    case Command::Quit:
        return false;
    case Command::PlayPause:
        if (engine != nullptr) {
            if (engine->state() == engine::State::Playing) {
                (void)engine->pause();
            } else {
                (void)engine->play();
            }
        }
        return true;
    case Command::SubmitInput: {
        // Command palette: only ':q' / ':quit' wired for now.
        if (input.mode() == Mode::Command) {
            const auto& b = input.buffer();
            if (b == "q" || b == "quit") {
                input.clear_transient();
                return false;
            }
        }
        input.clear_transient();
        return true;
    }
    case Command::Cancel:
        input.clear_transient();
        return true;
    default:
        return true;
    }
}

} // namespace

int run(engine::Engine* engine, library::Library* /*library*/) {
    struct notcurses* nc = current_context();
    if (nc == nullptr) {
        return -1;
    }

    InputMap input;
    render_frame(nc, engine, input);

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
            render_frame(nc, engine, input);
            continue;
        }

        const CommandResult cr = input.handle(ni);
        if (!dispatch_command(cr, engine, input)) {
            break;
        }
        render_frame(nc, engine, input);
    }
    return 0;
}

} // namespace transporter::tui
