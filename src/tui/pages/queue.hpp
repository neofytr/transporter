// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_TUI_PAGES_QUEUE_HPP
#define TRANSPORTER_TUI_PAGES_QUEUE_HPP

#include <cstddef>
#include <vector>

#include <transporter/library/library.hpp>

struct ncplane;

namespace transporter::tui::pages {

// Queue page: vertical list with the current track marked '▶', selection
// cursor, and an empty-state prompt when the queue has no tracks.
class QueuePage {
public:
    QueuePage() = default;

    // Render the queue. `queue` is the live track list; `queue_idx` marks the
    // currently-playing row.
    void draw(struct ncplane* host, int body_y0, int body_rows,
              const std::vector<library::Track>& queue,
              std::size_t queue_idx);

    // Selection cursor navigation. Bounded by queue size; returns true when
    // the selection actually moved.
    bool move_up(int count, std::size_t size);
    bool move_down(int count, std::size_t size);
    bool goto_top(std::size_t size);
    bool goto_bottom(std::size_t size);
    bool half_page_down(std::size_t size);
    bool half_page_up(std::size_t size);

    std::size_t selected() const noexcept { return selected_; }
    void set_selected(std::size_t s) noexcept { selected_ = s; }

    // Last rendered row count for half-page jumps and scroll math.
    int last_visible() const noexcept { return last_visible_; }

    // Translate a body-space click row to a track index. Returns SIZE_MAX
    // when the click was outside the list rows.
    std::size_t hit_row(int y) const noexcept;

private:
    std::size_t selected_ = 0;
    std::size_t scroll_   = 0;
    int last_visible_ = 0;
    int list_y0_     = -1;
};

} // namespace transporter::tui::pages

#endif
