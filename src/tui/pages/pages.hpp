// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_TUI_PAGES_PAGES_HPP
#define TRANSPORTER_TUI_PAGES_PAGES_HPP

#include <string_view>

struct ncplane;

namespace transporter::tui::pages {

// Draw a page stub into the body region of `host`. body_y0 is the top row,
// body_rows is the height available (everything above the player bar). All
// stubs render a rounded border, the page title in the top edge, a centered
// title in the body, and a one-line subtitle.

void draw_pipeline(struct ncplane* host, int body_y0, int body_rows);
void draw_settings(struct ncplane* host, int body_y0, int body_rows);

// Status hint strip — one line above the player bar showing keys relevant
// to the active page. `subview` is non-zero for sub-pages within the page
// (currently: 1 = album detail when active_page == Library).
std::string_view hint_for(int page_id, int subview = 0);

} // namespace transporter::tui::pages

#endif
