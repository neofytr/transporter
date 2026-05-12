// SPDX-License-Identifier: GPL-3.0-or-later

#include "transport.hpp"

#include <notcurses/notcurses.h>

#include <cstdint>

namespace transporter::tui::components {

namespace {

constexpr int kZoneWidth = 3;
constexpr int kGap       = 3;

void set_rgb24(struct ncplane* p, std::uint32_t rgb) {
    const auto r = static_cast<std::uint8_t>((rgb >> 16) & 0xFFu);
    const auto g = static_cast<std::uint8_t>((rgb >>  8) & 0xFFu);
    const auto b = static_cast<std::uint8_t>(rgb         & 0xFFu);
    ncplane_set_fg_rgb8(p, r, g, b);
}

} // namespace

TransportHit hit_test_transport(int rel_x) {
    if (rel_x < 0 || rel_x >= kTransportWidth) {
        return TransportHit::None;
    }
    // Layout: [Prev:3][gap:3][PlayPause:3][gap:3][Next:3]
    if (rel_x < kZoneWidth) {
        return TransportHit::Prev;
    }
    if (rel_x < kZoneWidth + kGap) {
        return TransportHit::None;
    }
    if (rel_x < kZoneWidth + kGap + kZoneWidth) {
        return TransportHit::PlayPause;
    }
    if (rel_x < kZoneWidth + kGap + kZoneWidth + kGap) {
        return TransportHit::None;
    }
    return TransportHit::Next;
}

void draw_transport(struct ncplane* dst, int y, int x, std::uint32_t fg_rgb) {
    if (dst == nullptr) {
        return;
    }
    set_rgb24(dst, fg_rgb);
    // ⏮ U+23EE, ⏯ U+23EF, ⏭ U+23ED. Each glyph is one cell; pad with two
    // trailing spaces so the 3-cell hit zone is consistent.
    ncplane_putstr_yx(dst, y, x + 0,  "\xE2\x8F\xAE");  // ⏮
    ncplane_putstr_yx(dst, y, x + 6,  "\xE2\x8F\xAF");  // ⏯
    ncplane_putstr_yx(dst, y, x + 12, "\xE2\x8F\xAD");  // ⏭
    ncplane_set_fg_default(dst);
}

} // namespace transporter::tui::components
