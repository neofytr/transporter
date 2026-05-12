// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.hpp"

#include "components/player_bar.hpp"
#include "input.hpp"
#include "notcurses_ctx.hpp"
#include "pages/pages.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/engine/telemetry.hpp>

#include <notcurses/notcurses.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace transporter::tui {

namespace {

constexpr int kPageCount = 5;

void draw_page(struct ncplane* p, PageId page, int body_y0, int body_rows) {
    switch (page) {
    case PageId::Library:
        pages::draw_library(p, body_y0, body_rows);
        break;
    case PageId::Queue:
        pages::draw_queue(p, body_y0, body_rows);
        break;
    case PageId::NowPlaying:
        pages::draw_now_playing(p, body_y0, body_rows);
        break;
    case PageId::Pipeline:
        pages::draw_pipeline(p, body_y0, body_rows);
        break;
    case PageId::Settings:
        pages::draw_settings(p, body_y0, body_rows);
        break;
    }
}

PageId page_next(PageId p) {
    int n = static_cast<int>(p) + 1;
    if (n > kPageCount) {
        n = 1;
    }
    return static_cast<PageId>(n);
}

PageId page_prev(PageId p) {
    int n = static_cast<int>(p) - 1;
    if (n < 1) {
        n = kPageCount;
    }
    return static_cast<PageId>(n);
}

struct AppState {
    PageId active_page = PageId::Library;
};

void render_frame(struct notcurses* nc, engine::Engine* engine,
                  const InputMap& input, const AppState& state) {
    struct ncplane* std_plane = notcurses_stdplane(nc);
    unsigned rows = 0, cols = 0;
    ncplane_dim_yx(std_plane, &rows, &cols);
    ncplane_erase(std_plane);

    const int body_rows = static_cast<int>(rows)
                        - components::kPlayerBarRows;
    if (body_rows > 0) {
        draw_page(std_plane, state.active_page, 0, body_rows);
    }

    if (engine != nullptr) {
        const auto snap = engine->pipeline_snapshot();
        const bool playing = engine->state() == engine::State::Playing;
        components::draw_player_bar(std_plane, &snap, /*volume_pct=*/65,
                                    playing);
    } else {
        components::draw_player_bar(std_plane, nullptr, 0, false);
    }

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
                      InputMap& input, AppState& state) {
    switch (cr.cmd) {
    case Command::Quit:
        return false;
    case Command::PageGoto: {
        const int n = static_cast<int>(cr.page_target);
        if (n >= 1 && n <= kPageCount) {
            state.active_page = cr.page_target;
        }
        return true;
    }
    case Command::PageNext:
        state.active_page = page_next(state.active_page);
        return true;
    case Command::PagePrev:
        state.active_page = page_prev(state.active_page);
        return true;
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
    AppState state;
    render_frame(nc, engine, input, state);

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
            render_frame(nc, engine, input, state);
            continue;
        }

        const CommandResult cr = input.handle(ni);
        if (!dispatch_command(cr, engine, input, state)) {
            break;
        }
        render_frame(nc, engine, input, state);
    }
    return 0;
}

} // namespace transporter::tui
