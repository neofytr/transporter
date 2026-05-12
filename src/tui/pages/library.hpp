// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_TUI_PAGES_LIBRARY_HPP
#define TRANSPORTER_TUI_PAGES_LIBRARY_HPP

#include "../components/cover_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct ncplane;

namespace transporter::library {
class Library;
struct Album;
} // namespace transporter::library

namespace transporter::tui::pages {

// Album-grid view backed by Library::albums(). Persistent selection state
// survives across page swaps so re-entering the page restores the cursor.
class LibraryPage {
public:
    explicit LibraryPage(library::Library* lib = nullptr);

    // Refresh the cached album list from the library. Called once on init and
    // when the library reports a delta. Safe with a null library (becomes a
    // no-op + empty list).
    void refresh();

    // Render into the body region of `host`. body_y0 is the top row available
    // for the page; body_rows is the height (between the title bar / above
    // the status hint strip). Honors selection state and computes a fitting
    // grid layout.
    void draw(struct ncplane* host, int body_y0, int body_rows);

    // Navigation. count is the prefix multiplier (e.g. 5j → count=5). The
    // page uses its cached column count from the last draw() to translate
    // up/down into row motion. Returns true when the selection changed.
    bool move_left(int count);
    bool move_right(int count);
    bool move_up(int count);
    bool move_down(int count);
    bool goto_top();
    bool goto_bottom();
    bool half_page_down();
    bool half_page_up();

    // Selected album id (the row's library id), or nullopt if empty / out
    // of range. Caller uses this to navigate to AlbumDetail on Enter.
    std::optional<std::int64_t> selected_album_id() const;

    std::size_t album_count() const noexcept { return albums_.size(); }

private:
    library::Library* lib_ = nullptr;
    std::vector<library::Album> albums_;
    components::CoverCache cover_cache_{50};

    std::size_t selected_ = 0;     // index into albums_
    std::size_t scroll_row_ = 0;   // top row of visible window
    int last_cols_ = 1;            // columns from last draw()
    int last_rows_ = 1;            // rows from last draw()

    void clamp_selection();
};

} // namespace transporter::tui::pages

#endif
