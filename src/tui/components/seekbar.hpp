// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_TUI_COMPONENTS_SEEKBAR_HPP
#define TRANSPORTER_TUI_COMPONENTS_SEEKBAR_HPP

#include <cstdint>
#include <string>

struct ncplane;

namespace transporter::tui::components {

// Visual styling for the seekbar. RGB packed as 0xRRGGBB. The defaults are
// the placeholder accent (steel/cyan) that the rest of the TUI uses until T9
// ports the album-art dominant-color extractor.
struct SeekbarStyle {
    std::uint32_t played_rgb   = 0x7AB8C8;  // accent
    std::uint32_t unplayed_rgb = 0x404040;  // dim
    std::uint32_t cursor_rgb   = 0xFFFFFF;  // white
    bool          draw_waveform = true;     // T3 placeholder; T6 wires real envelope
};

// Map a click column (relative to the seekbar's leftmost cell) to a cell
// index within `width`. Negative columns clamp to 0; columns past the end
// clamp to width-1. `width` must be positive; for non-positive width the
// function returns 0.
int hit_test(int col, int width);

// Translate a cell offset within the seekbar to a ratio in [0, 1]. Used by
// the page input handler to compute a seek target in milliseconds.
double position_ratio(int col, int width);

// Format "MM:SS / MM:SS" from millisecond inputs. Duration zero produces
// "0:00 / 0:00".
std::string format_time(std::int64_t position_ms, std::int64_t duration_ms);

// Draw the seekbar at row `y`, starting at column `x`, spanning `width`
// cells. The seekbar itself occupies row `y`. When `waveform_height > 0`
// the placeholder braille envelope is rendered on the rows immediately
// below (`y+1` ... `y+waveform_height`).
//
// position_ms / duration_ms drive the cursor and played portion; out-of-
// range values are clamped. `duration_ms <= 0` renders an empty bar.
void draw_seekbar(struct ncplane* dst, int y, int x, int width,
                  std::int64_t position_ms, std::int64_t duration_ms,
                  int waveform_height, const SeekbarStyle& style);

} // namespace transporter::tui::components

#endif
