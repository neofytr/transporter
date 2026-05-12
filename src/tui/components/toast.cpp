// SPDX-License-Identifier: GPL-3.0-or-later

#include "toast.hpp"

#include <notcurses/notcurses.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace transporter::tui::components {

void Toast::trigger(std::string text,
                    std::chrono::steady_clock::time_point now) {
    text_ = std::move(text);
    triggered_ = now;
}

bool Toast::visible(std::chrono::steady_clock::time_point now) const noexcept {
    if (text_.empty()) {
        return false;
    }
    if (triggered_.time_since_epoch().count() == 0) {
        return false;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - triggered_);
    return elapsed >= std::chrono::milliseconds{0} && elapsed < kTotal;
}

void Toast::draw(struct ncplane* host, int y, int cols,
                 std::chrono::steady_clock::time_point now) const {
    if (host == nullptr || cols <= 0 || !visible(now)) {
        return;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - triggered_);

    // Brightness: full during the hold window, ramp down to ~25% during fade.
    std::uint8_t v = 0xE0;
    if (elapsed > kHold) {
        const auto into_fade = elapsed - kHold;
        const double t = static_cast<double>(into_fade.count())
                       / static_cast<double>(kFade.count());
        const double brightness = std::max(0.25, 1.0 - t * 0.75);
        v = static_cast<std::uint8_t>(brightness * 0xE0);
    }

    ncplane_set_fg_rgb8(host, v, v, v);
    // Clear the row, then paint the message left-aligned with a 1-cell pad.
    for (int x = 0; x < cols; ++x) {
        ncplane_putchar_yx(host, y, x, ' ');
    }
    const int max = cols - 2;
    if (max > 0) {
        // Byte-truncate at a UTF-8 boundary.
        std::string clipped = text_;
        if (static_cast<int>(clipped.size()) > max) {
            auto cut = static_cast<std::size_t>(max);
            while (cut > 0
                   && (static_cast<unsigned char>(clipped[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            clipped.resize(cut);
        }
        ncplane_putstr_yx(host, y, 1, clipped.c_str());
    }
    ncplane_set_fg_default(host);
}

} // namespace transporter::tui::components
