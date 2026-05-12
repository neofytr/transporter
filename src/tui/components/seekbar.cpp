// SPDX-License-Identifier: GPL-3.0-or-later

#include "seekbar.hpp"

#include <notcurses/notcurses.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace transporter::tui::components {

namespace {

void set_rgb24(struct ncplane* p, std::uint32_t rgb) {
    const auto r = static_cast<std::uint8_t>((rgb >> 16) & 0xFFu);
    const auto g = static_cast<std::uint8_t>((rgb >>  8) & 0xFFu);
    const auto b = static_cast<std::uint8_t>(rgb         & 0xFFu);
    ncplane_set_fg_rgb8(p, r, g, b);
}

std::string mm_ss(std::int64_t ms) {
    if (ms <= 0) {
        return "0:00";
    }
    const long long total_s = static_cast<long long>(ms / 1000);
    const long long s = total_s % 60;
    const long long m = (total_s / 60) % 60;
    const long long h = total_s / 3600;
    std::array<char, 32> buf{};
    if (h > 0) {
        std::snprintf(buf.data(), buf.size(), "%lld:%02lld:%02lld", h, m, s);
    } else {
        std::snprintf(buf.data(), buf.size(), "%lld:%02lld", m, s);
    }
    return std::string(buf.data());
}

// Static fake envelope. Eight-glyph cycle of dual-density braille spanning
// peak + RMS amplitudes. T6 replaces this with the real per-track decode.
constexpr std::string_view kWaveform[] = {
    "\xE2\xA0\x87", // ⠇
    "\xE2\xA0\x9E", // ⠞
    "\xE2\xA0\xA9", // ⠩
    "\xE2\xA0\x8B", // ⠋
    "\xE2\xA0\x87", // ⠇
    "\xE2\xA0\x9E", // ⠞
    "\xE2\xA0\xA9", // ⠩
    "\xE2\xA0\x8B", // ⠋
};

} // namespace

int hit_test(int col, int width) {
    if (width <= 0) {
        return 0;
    }
    if (col <= 0) {
        return 0;
    }
    if (col >= width) {
        return width - 1;
    }
    return col;
}

double position_ratio(int col, int width) {
    if (width <= 1) {
        return 0.0;
    }
    const int c = hit_test(col, width);
    return static_cast<double>(c) / static_cast<double>(width - 1);
}

std::string format_time(std::int64_t position_ms, std::int64_t duration_ms) {
    return mm_ss(position_ms) + " / " + mm_ss(duration_ms);
}

void draw_seekbar(struct ncplane* dst, int y, int x, int width,
                  std::int64_t position_ms, std::int64_t duration_ms,
                  int waveform_height, const SeekbarStyle& style) {
    if (dst == nullptr || width <= 0) {
        return;
    }

    double progress = 0.0;
    if (duration_ms > 0) {
        progress = static_cast<double>(position_ms) /
                   static_cast<double>(duration_ms);
    }
    progress = std::clamp(progress, 0.0, 1.0);

    int filled = static_cast<int>(static_cast<double>(width) * progress);
    if (filled > width) {
        filled = width;
    }

    // Played portion (accent).
    set_rgb24(dst, style.played_rgb);
    for (int i = 0; i < filled; ++i) {
        ncplane_putstr_yx(dst, y, x + i, "\xE2\x94\x81"); // ━
    }

    // Cursor at the head, when there's room.
    if (filled < width) {
        set_rgb24(dst, style.cursor_rgb);
        ncplane_putstr_yx(dst, y, x + filled, "\xE2\x97\x8F"); // ●
        set_rgb24(dst, style.unplayed_rgb);
        for (int i = filled + 1; i < width; ++i) {
            ncplane_putstr_yx(dst, y, x + i, "\xE2\x94\x80"); // ─
        }
    }

    // placeholder waveform; T6 wires real envelope
    if (style.draw_waveform && waveform_height > 0) {
        set_rgb24(dst, style.unplayed_rgb);
        constexpr int kCycle = sizeof(kWaveform) / sizeof(kWaveform[0]);
        for (int r = 0; r < waveform_height; ++r) {
            for (int i = 0; i < width; ++i) {
                const auto& g = kWaveform[(i + r) % kCycle];
                ncplane_putstr_yx(dst, y + 1 + r, x + i, g.data());
            }
        }
    }

    ncplane_set_fg_default(dst);
}

} // namespace transporter::tui::components
