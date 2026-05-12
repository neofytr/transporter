// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_TUI_COMPONENTS_TRANSPORT_HPP
#define TRANSPORTER_TUI_COMPONENTS_TRANSPORT_HPP

#include <cstdint>

struct ncplane;

namespace transporter::tui::components {

// Three-button transport row: ⏮  ⏯  ⏭. The play/pause glyph is the
// universal toggle (⏯) per the locked spec.

// Click-zone identifiers returned by hit_test_transport.
enum class TransportHit : std::uint8_t {
    None,
    Prev,
    PlayPause,
    Next,
};

// Total cell width of the rendered transport row: three 3-cell zones with
// two 3-cell gaps between them. Centring is the caller's job.
constexpr int kTransportWidth = 15;

// Each icon gets a 3-cell hit zone with 3-cell gaps. Given an x relative
// to the leftmost cell of the transport row, returns which button (if any)
// was clicked.
TransportHit hit_test_transport(int rel_x);

// Draw the row at (y, x). Width is fixed at kTransportWidth cells.
void draw_transport(struct ncplane* dst, int y, int x,
                    std::uint32_t fg_rgb = 0xE0E0E0);

} // namespace transporter::tui::components

#endif
