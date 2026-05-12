// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.hpp"

#include "components/player_bar.hpp"
#include "components/toast.hpp"
#include "input.hpp"
#include "notcurses_ctx.hpp"
#include "pages/album_detail.hpp"
#include "pages/library.hpp"
#include "pages/now_playing.hpp"
#include "pages/pages.hpp"
#include "pages/queue.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/engine/telemetry.hpp>
#include <transporter/library/library.hpp>

#include <notcurses/notcurses.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace transporter::tui {

namespace {

constexpr int kPageCount = 5;
constexpr std::int64_t kSeekStepMs = 5000;

struct AppState {
    PageId active_page = PageId::Library;
    pages::LibraryPage library_page;
    pages::AlbumDetailPage album_detail;
    pages::NowPlayingPage now_playing;
    pages::QueuePage queue_page;
    std::optional<std::int64_t> viewing_album;

    // App-level queue. The Library/AlbumDetail layer replaces it on Enter;
    // the Queue page (T4) supports in-place reorder / remove / clear.
    std::vector<library::Track> queue;
    std::size_t                 queue_idx = 0;

    // Track-change detection drives the toast.
    std::string                 last_source_path;
    components::Toast           toast;

    AppState(library::Library* lib, engine::Engine* eng)
        : library_page(lib), album_detail(lib, eng), now_playing(eng) {}

    bool in_album_detail() const noexcept {
        return viewing_album.has_value();
    }
};

// Pull a fresh snapshot for handlers that need engine state to compute their
// effect (mouse hit-test on the seekbar, keyboard seek).
engine::PipelineSnapshot current_snap(engine::Engine* engine) {
    if (engine == nullptr) {
        return {};
    }
    return engine->pipeline_snapshot();
}

// Start playback of the track at queue[idx]. Updates queue_idx on success
// but leaves the queue untouched. Returns true on success.
bool play_at(engine::Engine* engine, AppState& state, std::size_t idx) {
    if (engine == nullptr || idx >= state.queue.size()) {
        return false;
    }
    auto lr = engine->load(state.queue[idx].path);
    if (!lr.has_value()) {
        return false;
    }
    auto pr = engine->play();
    if (!pr.has_value()) {
        return false;
    }
    state.queue_idx = idx;
    return true;
}

void draw_page(struct ncplane* p, AppState& state, int body_y0, int body_rows,
               const engine::PipelineSnapshot* snap) {
    switch (state.active_page) {
    case PageId::Library:
        if (state.in_album_detail()) {
            state.album_detail.draw(p, body_y0, body_rows);
        } else {
            state.library_page.draw(p, body_y0, body_rows);
        }
        break;
    case PageId::Queue:
        state.queue_page.draw(p, body_y0, body_rows,
                              state.queue, state.queue_idx);
        break;
    case PageId::NowPlaying:
        state.now_playing.draw(p, body_y0, body_rows, snap);
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

    // Snapshot the engine once per frame; pass it to NowPlaying + player bar.
    std::optional<engine::PipelineSnapshot> snap_opt;
    if (engine != nullptr) {
        snap_opt = engine->pipeline_snapshot();
    }
    const engine::PipelineSnapshot* snap_ptr =
        snap_opt.has_value() ? &snap_opt.value() : nullptr;

    // Track-change detection drives the toast.
    if (snap_ptr != nullptr) {
        const std::string& src = snap_ptr->source.file_path;
        if (!src.empty() && src != state.last_source_path) {
            const auto& tags = snap_ptr->source.tags;
            std::string title = tags.title.empty() ? src : tags.title;
            std::string msg = "\xE2\x96\xB6 Now playing: " + title;
            if (!tags.artist.empty()) {
                msg += " \xE2\x80\x94 ";  // —
                msg += tags.artist;
            }
            state.toast.trigger(std::move(msg),
                                std::chrono::steady_clock::now());
            state.last_source_path = src;
        }
    }

    const int body_rows = static_cast<int>(rows)
                        - components::kPlayerBarRows - kHintRows;
    if (body_rows > 0) {
        draw_page(std_plane, state, 0, body_rows, snap_ptr);
    }
    const int hint_y = static_cast<int>(rows)
                     - components::kPlayerBarRows - kHintRows;
    if (hint_y >= 0) {
        const int subview =
            (state.active_page == PageId::Library && state.in_album_detail())
                ? 1 : 0;
        draw_hint(std_plane, hint_y, static_cast<int>(cols),
                  pages::hint_for(static_cast<int>(state.active_page),
                                  subview));
    }

    // Toast paints inside the frame, on the row just above the bottom border.
    const auto now = std::chrono::steady_clock::now();
    if (state.toast.visible(now) && body_rows > 2) {
        const int toast_y = body_rows - 2;
        state.toast.draw(std_plane, toast_y, static_cast<int>(cols), now);
    }

    if (snap_ptr != nullptr) {
        const bool playing = engine->state() == engine::State::Playing;
        components::draw_player_bar(std_plane, snap_ptr, /*volume_pct=*/65,
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
        if (state.active_page == PageId::NowPlaying) {
            const auto s = current_snap(engine);
            state.now_playing.seek_relative(-kSeekStepMs, &s);
        } else if (state.active_page == PageId::Library
                   && !state.in_album_detail()) {
            state.library_page.move_left(cr.count);
        }
        return true;
    case Command::MoveRight:
        if (state.active_page == PageId::NowPlaying) {
            const auto s = current_snap(engine);
            state.now_playing.seek_relative(kSeekStepMs, &s);
        } else if (state.active_page == PageId::Library
                   && !state.in_album_detail()) {
            state.library_page.move_right(cr.count);
        }
        return true;
    case Command::MoveUp:
        if (state.active_page == PageId::Library) {
            if (state.in_album_detail()) {
                state.album_detail.move_up(cr.count);
            } else {
                state.library_page.move_up(cr.count);
            }
        } else if (state.active_page == PageId::Queue) {
            state.queue_page.move_up(cr.count, state.queue.size());
        }
        return true;
    case Command::MoveDown:
        if (state.active_page == PageId::Library) {
            if (state.in_album_detail()) {
                state.album_detail.move_down(cr.count);
            } else {
                state.library_page.move_down(cr.count);
            }
        } else if (state.active_page == PageId::Queue) {
            state.queue_page.move_down(cr.count, state.queue.size());
        }
        return true;
    case Command::GotoTop:
        if (state.active_page == PageId::Library) {
            if (state.in_album_detail()) {
                state.album_detail.goto_top();
            } else {
                state.library_page.goto_top();
            }
        } else if (state.active_page == PageId::Queue) {
            state.queue_page.goto_top(state.queue.size());
        }
        return true;
    case Command::GotoBottom:
        if (state.active_page == PageId::Library) {
            if (state.in_album_detail()) {
                state.album_detail.goto_bottom();
            } else {
                state.library_page.goto_bottom();
            }
        } else if (state.active_page == PageId::Queue) {
            state.queue_page.goto_bottom(state.queue.size());
        }
        return true;
    case Command::ScrollDown:
        if (state.active_page == PageId::Library) {
            if (state.in_album_detail()) {
                state.album_detail.half_page_down();
            } else {
                state.library_page.half_page_down();
            }
        } else if (state.active_page == PageId::Queue) {
            state.queue_page.half_page_down(state.queue.size());
        }
        return true;
    case Command::ScrollUp:
        if (state.active_page == PageId::Library) {
            if (state.in_album_detail()) {
                state.album_detail.half_page_up();
            } else {
                state.library_page.half_page_up();
            }
        } else if (state.active_page == PageId::Queue) {
            state.queue_page.half_page_up(state.queue.size());
        }
        return true;
    case Command::Activate:
        if (state.active_page == PageId::Library) {
            if (state.in_album_detail()) {
                // Replace the queue with this album's tracks, play at the
                // selected index, and jump to Now Playing.
                const auto& tracks = state.album_detail.tracks();
                const std::size_t idx = state.album_detail.selected_index();
                if (!tracks.empty() && idx < tracks.size()
                    && engine != nullptr) {
                    state.queue = tracks;
                    if (play_at(engine, state, idx)) {
                        state.viewing_album = std::nullopt;
                        state.album_detail.set_album(std::nullopt);
                        state.active_page = PageId::NowPlaying;
                    }
                }
            } else {
                auto sel = state.library_page.selected_album_id();
                if (sel.has_value()) {
                    state.viewing_album = sel;
                    state.album_detail.set_album(sel);
                }
            }
        } else if (state.active_page == PageId::Queue) {
            const std::size_t idx = state.queue_page.selected();
            if (idx < state.queue.size() && engine != nullptr) {
                (void)play_at(engine, state, idx);
            }
        }
        return true;
    case Command::QueueAppend:
        if (state.active_page == PageId::Library && state.in_album_detail()) {
            state.album_detail.append_selected();
        }
        return true;
    case Command::QueuePlayNext:
        if (state.active_page == PageId::Library && state.in_album_detail()) {
            state.album_detail.play_selected_next();
        }
        return true;
    case Command::NextTrack:
        if (engine != nullptr && !state.queue.empty()
            && state.queue_idx + 1 < state.queue.size()) {
            (void)play_at(engine, state, state.queue_idx + 1);
        }
        return true;
    case Command::PrevTrack:
        if (engine != nullptr && !state.queue.empty() && state.queue_idx > 0) {
            (void)play_at(engine, state, state.queue_idx - 1);
        }
        return true;
    case Command::MouseClick:
        if (state.active_page == PageId::NowPlaying && engine != nullptr) {
            const auto s = current_snap(engine);
            // Mouse y/x is screen-absolute; NowPlaying drew into the body
            // starting at row 0, so the coordinates already match.
            (void)state.now_playing.handle_mouse(cr.y, cr.x, &s);
        } else if (state.active_page == PageId::Queue
                   && !state.queue.empty()) {
            // Translate the row click to a track index; clicking selects.
            const std::size_t hit = state.queue_page.hit_row(cr.y);
            if (hit < state.queue.size()) {
                state.queue_page.set_selected(hit);
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
        if (input.mode() != Mode::Normal) {
            input.clear_transient();
            return true;
        }
        // Normal-mode ESC pops a sub-view (album_detail -> grid).
        if (state.in_album_detail()) {
            state.viewing_album = std::nullopt;
            state.album_detail.set_album(std::nullopt);
        }
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
    AppState state(library, engine);
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
