// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_TUI_COMPONENTS_PLAYER_BAR_HPP
#define TRANSPORTER_TUI_COMPONENTS_PLAYER_BAR_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

struct ncplane;

namespace transporter::engine {
struct PipelineSnapshot;
} // namespace transporter::engine

namespace transporter::tui::components {

// Fixed total height: cover thumb + three info rows.
constexpr int kPlayerBarRows = 4;

// Format a millisecond duration as "M:SS" or "H:MM:SS" when >=1h. Negative
// or zero produces "0:00". Used by the seekbar time label.
std::string format_mm_ss(std::chrono::milliseconds d);

// Format the sample-rate / bit-depth / codec triple used in the telemetry
// row, e.g. "44.1k/16 FLAC". Empty codec or zero rate yields "—".
std::string format_rate_bits_codec(std::uint32_t sample_rate_hz,
                                   std::uint16_t bit_depth,
                                   std::string_view codec);

// Format a fractional fill (0-1) as "ring NN%". Clamps out-of-range input.
std::string format_ring_fill(double fraction);

// Render the 4-row bar at the bottom of `host`. `host` is expected to be
// the standard plane; the bar consumes the bottom kPlayerBarRows rows.
void draw_player_bar(struct ncplane* host,
                     const engine::PipelineSnapshot* snap,
                     int volume_pct,
                     bool playing);

} // namespace transporter::tui::components

#endif
