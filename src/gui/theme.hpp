// SPDX-License-Identifier: GPL-3.0-or-later
//
// Apple-Music-style ImGui style applied per dominant colour. Called once at
// init with the default palette; reapplied on track change with the new art's
// muted base tint.

#ifndef TRANSPORTER_GUI_THEME_HPP
#define TRANSPORTER_GUI_THEME_HPP

#include "util/dominant_color.hpp"

namespace transporter::gui {

// Mutates ImGui::GetStyle() — spacing, rounding, colour table.
void apply_theme(const DominantColor& dc);

} // namespace transporter::gui

#endif
