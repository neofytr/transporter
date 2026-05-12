// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_TUI_PAGES_NOW_PLAYING_HPP
#define TRANSPORTER_TUI_PAGES_NOW_PLAYING_HPP

#include "../components/cover_cache.hpp"
#include "../components/seekbar.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

struct ncplane;

namespace transporter::engine {
class Engine;
struct PipelineSnapshot;
} // namespace transporter::engine

namespace transporter::tui::pages {

// Now Playing page: cover left, info/seekbar/transport right.
class NowPlayingPage {
public:
    explicit NowPlayingPage(engine::Engine* eng = nullptr);

    // Draw into the body region of `host`. The page reads engine state via
    // `snap`; passing nullptr renders the empty-state.
    void draw(struct ncplane* host, int body_y0, int body_rows,
              const engine::PipelineSnapshot* snap);

    // Mouse click translated to cell coordinates within the body. Returns
    // true when the click was on a known hit zone.
    bool handle_mouse(int y, int x,
                      const engine::PipelineSnapshot* snap);

    // ±5 second seek triggered by 'h' / 'l'. delta_ms may be negative.
    void seek_relative(std::int64_t delta_ms,
                       const engine::PipelineSnapshot* snap);

private:
    engine::Engine*               eng_ = nullptr;
    components::CoverCache        cover_cache_{20};

    // Cached layout from the last draw(). Used by handle_mouse to map a
    // click coordinate to the seekbar / transport.
    int seek_y_  = -1;
    int seek_x_  = -1;
    int seek_w_  =  0;
    int transport_y_ = -1;
    int transport_x_ = -1;

    // Last seen track path; reset cached cover plane handle when it changes.
    std::filesystem::path last_path_;
};

} // namespace transporter::tui::pages

#endif
