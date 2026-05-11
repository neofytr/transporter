// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pixel-average → muted dominant colour extraction from album art.
// Caches results by track id (LRU, 50 entries).

#ifndef TRANSPORTER_GUI_UTIL_DOMINANT_COLOR_HPP
#define TRANSPORTER_GUI_UTIL_DOMINANT_COLOR_HPP

#include <imgui.h>

#include <cstdint>
#include <string>

namespace transporter::gui {

struct AlbumArt;

struct DominantColor {
    // Saturated 25%, value 20% — base for ImGuiCol_WindowBg / ChildBg.
    ImVec4 muted_bg{0.058f, 0.058f, 0.070f, 1.0f};
    // Saturated 55%, value 65% — accent for sliders, progress, active glyphs.
    ImVec4 accent{0.470f, 0.549f, 0.941f, 1.0f};
    // Always near-white. Readable against any dark muted background.
    ImVec4 text{0.980f, 0.980f, 0.988f, 1.0f};
    // Dimmed text for secondary lines (~55% white).
    ImVec4 text_dim{0.620f, 0.620f, 0.660f, 1.0f};
};

// Sensible neutral when no album art is available.
DominantColor default_dominant_color();

// Computes the muted dominant colour from `art`. Result is cached against
// `track_key` (typically the track path); a repeat call with the same key
// returns the cached entry. LRU cap of 50 entries.
// `art` may be empty — returns default_dominant_color() in that case.
DominantColor compute_dominant(const std::string& track_key, const AlbumArt& art);

} // namespace transporter::gui

#endif
