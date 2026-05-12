// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_TUI_PAGES_DRAW_UTIL_HPP
#define TRANSPORTER_TUI_PAGES_DRAW_UTIL_HPP

#include <cstddef>
#include <string>
#include <string_view>

struct ncplane;

namespace transporter::tui::pages {

// Rounded-light frame ("╭─ Title ─...─╮") around the rect (y0..y0+rows-1)
// across the full width of `host`. Title is embedded into the top edge.
void draw_frame(struct ncplane* host, int y0, int rows,
                std::string_view title);

// Center a string horizontally on row y, within the full width of `host`.
void put_centered(struct ncplane* host, int y, std::string_view s);

// Truncate `s` to at most max_cells cells, appending '…' when it would
// otherwise overflow. Byte-counted (good for ASCII); on UTF-8 input it
// walks back to a codepoint boundary before applying the ellipsis.
std::string clip_with_ellipsis(std::string_view s, std::size_t max_cells);

} // namespace transporter::tui::pages

#endif
