// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.hpp"

#include "components/player_bar.hpp"
#include "input.hpp"
#include "notcurses_ctx.hpp"
#include "pages/library.hpp"
#include "pages/pages.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/engine/telemetry.hpp>
#include <transporter/library/library.hpp>

#include <notcurses/notcurses.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace transporter::tui {

namespace {

constexpr int kPageCount = 5;

struct AppState {
    PageId active_page = PageId::Library;
    pages::LibraryPage library_page;

    explicit AppState(library::Library* lib) : library_page(lib) {}
};

void draw_page(struct ncplane* p, AppState& state, int body_y0, int body_rows) {
    switch (state.active_page) {
    case PageId::Library:
        state.library_page.draw(p, body_y0, body_rows);
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

constexpr int kHintRows = 1;
constexpr int kMinCols = 60;
constexpr int kMinRows = 20;

void draw_too_small(struct ncplane* host, int rows, int cols) {
    ncplane_erase(host);
    constexpr std::string_view msg = "terminal too small — resize or use --daemon";
    const int y = rows / 2;
    const int x = (cols - static_cast<int>(msg.size())) / 2;
    if (y >= 0 && x >= 0) {
        ncplane_putstr_yx(host, y, x, msg.data());
    }
    std::array<char, 32> dims{};
    std::snprintf(dims.data(), dims.size(), "(%d x %d, need 60 x 20)",
                  cols, rows);
    const int x2 = (cols - static_cast<int>(std::strlen(dims.data()))) / 2;
    if (y + 1 < rows && x2 >= 0) {
        ncplane_set_fg_rgb8(host, 0x70, 0x70, 0x70);
        ncplane_putstr_yx(host, y + 1, x2, dims.data());
        ncplane_set_fg_default(host);
    }
}

void draw_hint(struct ncplane* host, int y, int cols, std::string_view hint) {
    ncplane_set_fg_rgb8(host, 0x70, 0x70, 0x70);
    for (int x = 0; x < cols; ++x) {
        ncplane_putchar_yx(host, y, x, ' ');
    }
    if (!hint.empty()) {
        ncplane_putstr_yx(host, y, 1, hint.data());
    }
    ncplane_set_fg_default(host);
}

void render_frame(struct notcurses* nc, engine::Engine* engine,
                  const InputMap& input, AppState& state) {
    struct ncplane* std_plane = notcurses_stdplane(nc);
    unsigned rows = 0, cols = 0;
    ncplane_dim_yx(std_plane, &rows, &cols);
    ncplane_erase(std_plane);

    if (static_cast<int>(rows) < kMinRows || static_cast<int>(cols) < kMinCols) {
        draw_too_small(std_plane, static_cast<int>(rows),
                       static_cast<int>(cols));
        notcurses_render(nc);
        return;
    }

    const int body_rows = static_cast<int>(rows)
                        - components::kPlayerBarRows - kHintRows;
    if (body_rows > 0) {
        draw_page(std_plane, state, 0, body_rows);
    }
    const int hint_y = static_cast<int>(rows)
                     - components::kPlayerBarRows - kHintRows;
    if (hint_y >= 0) {
        draw_hint(std_plane, hint_y, static_cast<int>(cols),
                  pages::hint_for(static_cast<int>(state.active_page)));
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
    case Command::MoveLeft:
        if (state.active_page == PageId::Library) {
            state.library_page.move_left(cr.count);
        }
        return true;
    case Command::MoveRight:
        if (state.active_page == PageId::Library) {
            state.library_page.move_right(cr.count);
        }
        return true;
    case Command::MoveUp:
        if (state.active_page == PageId::Library) {
            state.library_page.move_up(cr.count);
        }
        return true;
    case Command::MoveDown:
        if (state.active_page == PageId::Library) {
            state.library_page.move_down(cr.count);
        }
        return true;
    case Command::GotoTop:
        if (state.active_page == PageId::Library) {
            state.library_page.goto_top();
        }
        return true;
    case Command::GotoBottom:
        if (state.active_page == PageId::Library) {
            state.library_page.goto_bottom();
        }
        return true;
    case Command::ScrollDown:
        if (state.active_page == PageId::Library) {
            state.library_page.half_page_down();
        }
        return true;
    case Command::ScrollUp:
        if (state.active_page == PageId::Library) {
            state.library_page.half_page_up();
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

int run(engine::Engine* engine, library::Library* library) {
    struct notcurses* nc = current_context();
    if (nc == nullptr) {
        return -1;
    }

    InputMap input;
    AppState state(library);
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
