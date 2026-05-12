// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_TUI_PAGES_ALBUM_DETAIL_HPP
#define TRANSPORTER_TUI_PAGES_ALBUM_DETAIL_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <transporter/library/library.hpp>

struct ncplane;

namespace transporter::engine {
class Engine;
} // namespace transporter::engine

namespace transporter::library {
class Library;
} // namespace transporter::library

namespace transporter::tui::pages {

// Album-detail view: cover hero + meta header + track list with disc
// separators when the album spans more than one disc.
class AlbumDetailPage {
public:
    explicit AlbumDetailPage(library::Library* lib = nullptr,
                             engine::Engine*  eng = nullptr);

    // Set the album id to render. Loads the track list synchronously.
    // Pass nullopt to clear (the page becomes a no-op).
    void set_album(std::optional<std::int64_t> album_id);

    std::optional<std::int64_t> album_id() const noexcept { return album_id_; }

    void draw(struct ncplane* host, int body_y0, int body_rows);

    bool move_up(int count);
    bool move_down(int count);
    bool goto_top();
    bool goto_bottom();
    bool half_page_down();
    bool half_page_up();

    // Enter on selected: replace queue and start playback at this track.
    // Returns true if a track was loaded.
    bool play_selected();

    // 'a' append; 'A' play-next. Stubs for the queue page; currently a no-op
    // beyond returning whether a track is selected (real queue lands in T4).
    bool append_selected();
    bool play_selected_next();

private:
    library::Library* lib_ = nullptr;
    engine::Engine*   eng_ = nullptr;
    std::optional<std::int64_t> album_id_;
    std::vector<library::Track> tracks_;
    std::size_t selected_ = 0;
    std::size_t scroll_   = 0;
    int last_visible_ = 0;
    std::string title_;
    std::string album_artist_;
    std::string date_;

    void reload();
    void clamp_selection();
};

} // namespace transporter::tui::pages

#endif
