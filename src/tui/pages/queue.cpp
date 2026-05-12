// SPDX-License-Identifier: GPL-3.0-or-later

#include "queue.hpp"

#include "draw_util.hpp"
#include "../components/player_bar.hpp"

#include <notcurses/notcurses.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace transporter::tui::pages {

namespace {

struct Rgb { std::uint8_t r, g, b; };
constexpr Rgb kAccent  {0x7A, 0xB8, 0xC8};
constexpr Rgb kDim     {0x90, 0x90, 0x90};
constexpr Rgb kSelBg   {0x2A, 0x40, 0x4A};
constexpr Rgb kPlayBg  {0x1E, 0x35, 0x42};

void set_rgb(struct ncplane* p, Rgb c) {
    ncplane_set_fg_rgb8(p, c.r, c.g, c.b);
}

void set_default(struct ncplane* p) {
    ncplane_set_fg_default(p);
}

std::string header_label(const std::vector<library::Track>& queue) {
    if (queue.empty()) {
        return "Queue (empty)";
    }
    std::array<char, 64> buf{};
    const char* noun = queue.size() == 1 ? "track" : "tracks";
    std::snprintf(buf.data(), buf.size(), "Queue (%zu %s)",
                  queue.size(), noun);
    return std::string(buf.data());
}

void draw_empty_state(struct ncplane* host, int body_y0, int body_rows) {
    if (body_rows < 4) {
        return;
    }
    const int mid = body_y0 + body_rows / 2;
    set_rgb(host, kDim);
    put_centered(host, mid - 1, "\xE2\x99\xAB"); // ♫
    put_centered(host, mid + 1,
                 "Queue empty — pick something from Library (1)");
    set_default(host);
}

} // namespace

void QueuePage::draw(struct ncplane* host, int body_y0, int body_rows,
                     const std::vector<library::Track>& queue,
                     std::size_t queue_idx) {
    draw_frame(host, body_y0, body_rows, "Queue");

    last_visible_ = 0;
    list_y0_ = -1;

    if (body_rows < 4) {
        return;
    }

    if (queue.empty()) {
        draw_empty_state(host, body_y0, body_rows);
        return;
    }

    unsigned hrows = 0, hcols = 0;
    ncplane_dim_yx(host, &hrows, &hcols);
    (void)hrows;
    const int width = static_cast<int>(hcols);
    if (width < 40) {
        return;
    }

    // Clamp selection in case the queue shrank since the last draw.
    if (selected_ >= queue.size()) {
        selected_ = queue.size() - 1;
    }

    // Header line: "Queue (N tracks)" two cells in, one row below the top
    // edge.
    const int hdr_y = body_y0 + 1;
    const std::string label = header_label(queue);
    set_rgb(host, kDim);
    ncplane_putstr_yx(host, hdr_y, 2,
                      clip_with_ellipsis(label,
                          static_cast<std::size_t>(width - 4)).c_str());
    set_default(host);

    // List region begins two rows below the title (header row + blank line).
    const int list_y0 = body_y0 + 3;
    const int list_rows_avail = body_y0 + body_rows - 1 - list_y0;
    if (list_rows_avail < 2) {
        return;
    }
    last_visible_ = list_rows_avail;
    list_y0_ = list_y0;

    // Keep the selection in view.
    if (selected_ < scroll_) {
        scroll_ = selected_;
    } else if (selected_
               >= scroll_ + static_cast<std::size_t>(list_rows_avail)) {
        scroll_ = selected_
                - static_cast<std::size_t>(list_rows_avail) + 1;
    }

    // Per-row layout: "▶  Title · Artist .................... MM:SS"
    constexpr int kMarkerW = 2;        // "▶ " or "  "
    constexpr int kGapAfterMarker = 1;
    constexpr int kRightPad = 2;
    int row_y = list_y0;
    for (std::size_t i = scroll_; i < queue.size(); ++i) {
        if (row_y >= body_y0 + body_rows - 1) {
            break;
        }
        const library::Track& t = queue[i];
        const bool playing = (i == queue_idx);
        const bool selected = (i == selected_);

        // Background fill: playing row stronger, selected row weaker (or
        // overlay both when they coincide).
        if (playing && selected) {
            ncplane_set_bg_rgb8(host, kSelBg.r, kSelBg.g, kSelBg.b);
        } else if (playing) {
            ncplane_set_bg_rgb8(host, kPlayBg.r, kPlayBg.g, kPlayBg.b);
        } else if (selected) {
            ncplane_set_bg_rgb8(host, kSelBg.r, kSelBg.g, kSelBg.b);
        }
        if (playing || selected) {
            for (int x = 1; x < width - 1; ++x) {
                ncplane_putchar_yx(host, row_y, x, ' ');
            }
        }

        // Marker.
        if (playing) {
            set_rgb(host, kAccent);
            ncplane_putstr_yx(host, row_y, 2, "\xE2\x96\xB6"); // ▶
        } else {
            set_default(host);
            ncplane_putstr_yx(host, row_y, 2, "  ");
        }

        const int text_x = 2 + kMarkerW + kGapAfterMarker;
        const std::string dur = components::format_mm_ss(t.duration);
        const int dur_w = static_cast<int>(dur.size());
        const int text_w = std::max(
            8, width - text_x - dur_w - kRightPad - 1);

        // Compose "title · artist".
        const std::string& artist =
            t.artist.empty() ? t.album_artist : t.artist;
        std::string label_str = t.title.empty()
            ? t.path.filename().string()
            : t.title;
        if (!artist.empty()) {
            label_str += " \xC2\xB7 ";  // ·
            label_str += artist;
        }
        if (playing) {
            set_rgb(host, kAccent);
        } else {
            set_default(host);
        }
        ncplane_putstr_yx(host, row_y, text_x,
                          clip_with_ellipsis(label_str,
                              static_cast<std::size_t>(text_w)).c_str());

        // Duration right-aligned. Dim when not the playing row.
        const int dur_x = width - kRightPad - dur_w;
        if (!playing) {
            set_rgb(host, kDim);
        }
        ncplane_putstr_yx(host, row_y, dur_x, dur.c_str());

        if (playing || selected) {
            ncplane_set_bg_default(host);
        }
        set_default(host);
        ++row_y;
    }
}

bool QueuePage::move_up(int count, std::size_t size) {
    if (size == 0) {
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

bool QueuePage::move_down(int count, std::size_t size) {
    if (size == 0) {
        return false;
    }
    const int n = std::max(1, count);
    std::size_t s = selected_;
    for (int i = 0; i < n && s + 1 < size; ++i) {
        ++s;
    }
    if (s == selected_) {
        return false;
    }
    selected_ = s;
    return true;
}

bool QueuePage::goto_top(std::size_t size) {
    if (size == 0 || selected_ == 0) {
        return false;
    }
    selected_ = 0;
    return true;
}

bool QueuePage::goto_bottom(std::size_t size) {
    if (size == 0 || selected_ + 1 == size) {
        return false;
    }
    selected_ = size - 1;
    return true;
}

bool QueuePage::half_page_down(std::size_t size) {
    return move_down(std::max(1, last_visible_ / 2), size);
}

bool QueuePage::half_page_up(std::size_t size) {
    return move_up(std::max(1, last_visible_ / 2), size);
}

std::size_t QueuePage::hit_row(int y) const noexcept {
    if (list_y0_ < 0 || last_visible_ <= 0) {
        return std::numeric_limits<std::size_t>::max();
    }
    if (y < list_y0_ || y >= list_y0_ + last_visible_) {
        return std::numeric_limits<std::size_t>::max();
    }
    return scroll_ + static_cast<std::size_t>(y - list_y0_);
}

} // namespace transporter::tui::pages
