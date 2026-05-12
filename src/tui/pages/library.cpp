// SPDX-License-Identifier: GPL-3.0-or-later

#include "library.hpp"

#include "draw_util.hpp"
#include "../components/cover.hpp"

#include <transporter/library/library.hpp>

#include <notcurses/notcurses.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace transporter::tui::pages {

namespace {

// Tile geometry. Target tile is ~14 cells wide × ~10 cells tall: 8-row cover
// + 2 text lines + 1 row of internal padding. Gap is the cell margin between
// tiles. Page padding is the inset from the outer rounded frame.
constexpr int kTileWidth   = 14;
constexpr int kCoverRows   = 6;   // rows reserved for the cover blit
constexpr int kTextRows    = 2;   // title + artist
constexpr int kTileHeight  = kCoverRows + kTextRows + 1; // +1 internal padding
constexpr int kTileGap     = 2;
constexpr int kPagePadding = 2;   // inside the rounded frame

// Accent color while T9 hasn't ported the dominant-color extractor yet.
struct Rgb { std::uint8_t r, g, b; };
constexpr Rgb kAccent{0x7A, 0xB8, 0xC8};
constexpr Rgb kDim   {0x90, 0x90, 0x90};

void set_rgb(struct ncplane* p, Rgb c) {
    ncplane_set_fg_rgb8(p, c.r, c.g, c.b);
}

void set_default(struct ncplane* p) {
    ncplane_set_fg_default(p);
}

int compute_columns(int avail_w) {
    if (avail_w < kTileWidth) {
        return 1;
    }
    // (cols * kTileWidth) + ((cols-1) * kTileGap) <= avail_w
    // cols <= (avail_w + kTileGap) / (kTileWidth + kTileGap)
    return std::max(1, (avail_w + kTileGap) / (kTileWidth + kTileGap));
}

int compute_rows(int avail_h) {
    if (avail_h < kTileHeight) {
        return 1;
    }
    return std::max(1, (avail_h + kTileGap) / (kTileHeight + kTileGap));
}

void draw_cover_placeholder(struct ncplane* host, int y, int x,
                            int w, int h, bool selected) {
    if (selected) {
        set_rgb(host, kAccent);
    } else {
        set_rgb(host, kDim);
    }
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            if (row == 0 && col == 0) {
                ncplane_putstr_yx(host, y, x, "╭");
            } else if (row == 0 && col == w - 1) {
                ncplane_putstr_yx(host, y, x + col, "╮");
            } else if (row == h - 1 && col == 0) {
                ncplane_putstr_yx(host, y + row, x, "╰");
            } else if (row == h - 1 && col == w - 1) {
                ncplane_putstr_yx(host, y + row, x + col, "╯");
            } else if (row == 0 || row == h - 1) {
                ncplane_putstr_yx(host, y + row, x + col, "─");
            } else if (col == 0 || col == w - 1) {
                ncplane_putstr_yx(host, y + row, x + col, "│");
            } else {
                ncplane_putchar_yx(host, y + row, x + col, ' ');
            }
        }
    }
    const int gx = x + w / 2;
    const int gy = y + h / 2;
    ncplane_putstr_yx(host, gy, gx, "♫");
    set_default(host);
}

} // namespace

std::filesystem::path LibraryPage::album_path(std::int64_t album_id) {
    auto it = album_track_path_.find(album_id);
    if (it != album_track_path_.end()) {
        return it->second;
    }
    if (lib_ == nullptr) {
        return {};
    }
    auto r = lib_->tracks_in_album(album_id);
    std::filesystem::path p;
    if (r.has_value() && !r.value().empty()) {
        p = r.value().front().path;
    }
    album_track_path_.emplace(album_id, p);
    return p;
}

LibraryPage::LibraryPage(library::Library* lib) : lib_(lib) {
    refresh();
}

void LibraryPage::refresh() {
    albums_.clear();
    if (lib_ == nullptr) {
        return;
    }
    auto r = lib_->albums(std::string{});
    if (r.has_value()) {
        albums_ = std::move(r.value());
    }
    clamp_selection();
}

void LibraryPage::clamp_selection() {
    if (albums_.empty()) {
        selected_ = 0;
        scroll_row_ = 0;
        return;
    }
    if (selected_ >= albums_.size()) {
        selected_ = albums_.size() - 1;
    }
}

void LibraryPage::draw(struct ncplane* host, int body_y0, int body_rows) {
    draw_frame(host, body_y0, body_rows, "Library");
    if (body_rows < 6) {
        return;
    }

    unsigned hcols = 0, hrows = 0;
    ncplane_dim_yx(host, &hrows, &hcols);
    (void)hrows;
    const int width = static_cast<int>(hcols);

    // Empty-library state — centered glyph + hint.
    if (albums_.empty()) {
        const int mid = body_y0 + body_rows / 2;
        set_rgb(host, kDim);
        put_centered(host, mid - 1, "♫");
        put_centered(host, mid + 1, "No music library yet — :add ~/Music");
        set_default(host);
        return;
    }

    // Body region inside the frame, with page padding.
    const int inner_x0 = 1 + kPagePadding;
    const int inner_y0 = body_y0 + 1 + 1;  // top border + 1-row spacer
    const int inner_w  = width - 2 - 2 * kPagePadding;
    const int inner_h  = body_rows - 1 - 1 - 1;  // top border + spacer + bottom border
    if (inner_w < kTileWidth || inner_h < kTileHeight) {
        set_rgb(host, kDim);
        put_centered(host, body_y0 + body_rows / 2,
                     "(resize for grid)");
        set_default(host);
        return;
    }

    const int cols = compute_columns(inner_w);
    const int rows = compute_rows(inner_h);
    last_cols_ = cols;
    last_rows_ = rows;

    // Total rows of albums in the grid.
    const std::size_t total_rows = (albums_.size() + static_cast<std::size_t>(cols) - 1)
                                  / static_cast<std::size_t>(cols);
    // Scroll so the selected row stays in view.
    const std::size_t sel_row = selected_ / static_cast<std::size_t>(cols);
    if (sel_row < scroll_row_) {
        scroll_row_ = sel_row;
    } else if (sel_row >= scroll_row_ + static_cast<std::size_t>(rows)) {
        scroll_row_ = sel_row - static_cast<std::size_t>(rows) + 1;
    }
    if (total_rows <= static_cast<std::size_t>(rows)) {
        scroll_row_ = 0;
    }

    // Draw the visible rows.
    for (int r = 0; r < rows; ++r) {
        const std::size_t row_idx = scroll_row_ + static_cast<std::size_t>(r);
        if (row_idx >= total_rows) {
            break;
        }
        for (int c = 0; c < cols; ++c) {
            const std::size_t album_idx =
                row_idx * static_cast<std::size_t>(cols) + static_cast<std::size_t>(c);
            if (album_idx >= albums_.size()) {
                break;
            }
            const int tile_x = inner_x0 + c * (kTileWidth + kTileGap);
            const int tile_y = inner_y0 + r * (kTileHeight + kTileGap);
            if (tile_y + kTileHeight > body_y0 + body_rows - 1) {
                break;
            }
            const bool selected = (album_idx == selected_);
            const library::Album& album = albums_[album_idx];

            // 1. Cover blit when we have an art file; otherwise placeholder.
            bool blitted = false;
            const std::filesystem::path tpath = album_path(album.id);
            if (!tpath.empty()) {
                auto cov = cover_cache_.get_or_load(tpath,
                    [&]{ return components::load_cover_for_track(tpath); });
                if (cov) {
                    ncvisual* vis = components::visual_from_cover(*cov);
                    if (vis != nullptr) {
                        if (components::blit_into(host, vis, tile_y, tile_x,
                                                  kCoverRows, kTileWidth) == 0) {
                            blitted = true;
                        }
                        ncvisual_destroy(vis);
                    }
                }
            }
            if (!blitted) {
                draw_cover_placeholder(host, tile_y, tile_x,
                                       kTileWidth, kCoverRows, selected);
            }

            // 2. Title.
            const int title_y = tile_y + kCoverRows;
            const int text_w  = kTileWidth;
            if (selected) {
                ncplane_set_styles(host, NCSTYLE_BOLD);
                set_rgb(host, kAccent);
            } else {
                set_default(host);
            }
            const std::string title = clip_with_ellipsis(album.title,
                static_cast<std::size_t>(text_w));
            ncplane_putstr_yx(host, title_y, tile_x, title.c_str());
            ncplane_set_styles(host, NCSTYLE_NONE);

            // 3. Artist (dim).
            set_rgb(host, kDim);
            const std::string artist = clip_with_ellipsis(album.album_artist,
                static_cast<std::size_t>(text_w));
            ncplane_putstr_yx(host, title_y + 1, tile_x, artist.c_str());
            set_default(host);
        }
    }
}

bool LibraryPage::move_left(int count) {
    if (albums_.empty()) {
        return false;
    }
    const int n = std::max(1, count);
    std::size_t s = selected_;
    for (int i = 0; i < n && s > 0; ++i) {
        --s;
    }
    if (s == selected_) {
        return false;
    }
    selected_ = s;
    return true;
}

bool LibraryPage::move_right(int count) {
    if (albums_.empty()) {
        return false;
    }
    const int n = std::max(1, count);
    std::size_t s = selected_;
    for (int i = 0; i < n && s + 1 < albums_.size(); ++i) {
        ++s;
    }
    if (s == selected_) {
        return false;
    }
    selected_ = s;
    return true;
}

bool LibraryPage::move_up(int count) {
    if (albums_.empty()) {
        return false;
    }
    const int n = std::max(1, count);
    const std::size_t step = static_cast<std::size_t>(std::max(1, last_cols_));
    std::size_t s = selected_;
    for (int i = 0; i < n; ++i) {
        if (s < step) {
            break;
        }
        s -= step;
    }
    if (s == selected_) {
        return false;
    }
    selected_ = s;
    return true;
}

bool LibraryPage::move_down(int count) {
    if (albums_.empty()) {
        return false;
    }
    const int n = std::max(1, count);
    const std::size_t step = static_cast<std::size_t>(std::max(1, last_cols_));
    std::size_t s = selected_;
    for (int i = 0; i < n; ++i) {
        const std::size_t next = s + step;
        if (next >= albums_.size()) {
            break;
        }
        s = next;
    }
    if (s == selected_) {
        return false;
    }
    selected_ = s;
    return true;
}

bool LibraryPage::goto_top() {
    if (albums_.empty() || selected_ == 0) {
        return false;
    }
    selected_ = 0;
    return true;
}

bool LibraryPage::goto_bottom() {
    if (albums_.empty() || selected_ + 1 == albums_.size()) {
        return false;
    }
    selected_ = albums_.size() - 1;
    return true;
}

bool LibraryPage::half_page_down() {
    const int step = std::max(1, last_cols_) * std::max(1, last_rows_ / 2);
    return move_down(step);
}

bool LibraryPage::half_page_up() {
    const int step = std::max(1, last_cols_) * std::max(1, last_rows_ / 2);
    return move_up(step);
}

std::optional<std::int64_t> LibraryPage::selected_album_id() const {
    if (albums_.empty() || selected_ >= albums_.size()) {
        return std::nullopt;
    }
    return albums_[selected_].id;
}

} // namespace transporter::tui::pages
