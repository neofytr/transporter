// SPDX-License-Identifier: GPL-3.0-or-later

#include "player_bar.hpp"

#include "../notcurses_ctx.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/engine/telemetry.hpp>

#include <notcurses/notcurses.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace transporter::tui::components {

namespace {

constexpr int kCoverCols = 6;            // 6-cell-wide cover thumb
constexpr int kInfoLeftGap = 1;          // gap between cover and info
constexpr std::string_view kEm = "—";    // em-dash placeholder

// Render a small ASCII placeholder cover at (y0, 0). 4 rows × kCoverCols cells.
void draw_cover_thumb(struct ncplane* p, int y0) {
    ncplane_putstr_yx(p, y0 + 0, 0, "┌────┐");
    ncplane_putstr_yx(p, y0 + 1, 0, "│    │");
    ncplane_putstr_yx(p, y0 + 2, 0, "│ ART│");
    ncplane_putstr_yx(p, y0 + 3, 0, "└────┘");
}

// Clip a UTF-8 string to at most `max_cells` columns (best-effort: counts
// bytes for ASCII, drops to byte-truncation for non-ASCII). Used by labels
// where exact display width matters but multi-cell glyphs are rare.
std::string clip(std::string_view s, std::size_t max_cells) {
    if (s.size() <= max_cells) {
        return std::string(s);
    }
    // Avoid mid-codepoint cuts: walk back to a valid UTF-8 start.
    std::size_t cut = max_cells;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    if (cut == 0) {
        return std::string{};
    }
    return std::string(s.substr(0, cut));
}

const char* bitperfect_label(engine::BitPerfectVerdict::Level lv) {
    using L = engine::BitPerfectVerdict::Level;
    switch (lv) {
    case L::Yes:       return "YES";
    case L::Qualified: return "QUAL";
    case L::No:        return "NO";
    }
    return "—";
}

const char* rt_mode_label(engine::rt::Mode m) {
    return m == engine::rt::Mode::Fifo ? "RT-FIFO" : "RT-OTHER";
}

void set_dim(struct ncplane* p) {
    ncplane_set_fg_rgb8(p, 0x90, 0x90, 0x90);
}

void set_default_fg(struct ncplane* p) {
    ncplane_set_fg_default(p);
}

void set_accent(struct ncplane* p) {
    // Steel / cyan neutral until T9 wires real dominant-color extraction.
    ncplane_set_fg_rgb8(p, 0x7A, 0xB8, 0xC8);
}

// Render the seekbar segment: heavy block for played, light for unplayed,
// cursor dot at the boundary. width must be > 0.
void render_seekbar(struct ncplane* p, int y, int x, int width,
                    double progress01) {
    const double clamped = std::clamp(progress01, 0.0, 1.0);
    const int filled = static_cast<int>(static_cast<double>(width) * clamped);
    set_accent(p);
    for (int i = 0; i < filled; ++i) {
        ncplane_putstr_yx(p, y, x + i, "━");
    }
    if (filled < width) {
        ncplane_putstr_yx(p, y, x + filled, "●");
    }
    set_dim(p);
    for (int i = filled + 1; i < width; ++i) {
        ncplane_putstr_yx(p, y, x + i, "─");
    }
    set_default_fg(p);
}

} // namespace

std::string format_mm_ss(std::chrono::milliseconds d) {
    if (d.count() <= 0) {
        return "0:00";
    }
    const auto total_s = static_cast<long long>(d.count() / 1000);
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

std::string format_rate_bits_codec(std::uint32_t sample_rate_hz,
                                   std::uint16_t bit_depth,
                                   std::string_view codec) {
    if (sample_rate_hz == 0 || codec.empty()) {
        return std::string(kEm);
    }
    std::array<char, 32> buf{};
    const double khz = static_cast<double>(sample_rate_hz) / 1000.0;
    if (sample_rate_hz % 1000 == 0) {
        std::snprintf(buf.data(), buf.size(), "%.0fk/%u %.*s",
                      khz, static_cast<unsigned>(bit_depth),
                      static_cast<int>(codec.size()), codec.data());
    } else {
        std::snprintf(buf.data(), buf.size(), "%.1fk/%u %.*s",
                      khz, static_cast<unsigned>(bit_depth),
                      static_cast<int>(codec.size()), codec.data());
    }
    return std::string(buf.data());
}

std::string format_ring_fill(double fraction) {
    const double clamped = std::clamp(fraction, 0.0, 1.0);
    const int pct = static_cast<int>(clamped * 100.0 + 0.5);
    std::array<char, 16> buf{};
    std::snprintf(buf.data(), buf.size(), "ring %d%%", pct);
    return std::string(buf.data());
}

void draw_player_bar(struct ncplane* host,
                     const engine::PipelineSnapshot* snap,
                     int volume_pct,
                     bool playing) {
    unsigned rows = 0, cols = 0;
    ncplane_dim_yx(host, &rows, &cols);
    if (rows < static_cast<unsigned>(kPlayerBarRows) || cols < 24) {
        return;
    }
    const int y0 = static_cast<int>(rows) - kPlayerBarRows;
    const int info_x = kCoverCols + kInfoLeftGap;
    const int info_w = static_cast<int>(cols) - info_x;
    if (info_w < 4) {
        return;
    }

    // Clear the bar region.
    for (int row = 0; row < kPlayerBarRows; ++row) {
        for (int col = 0; col < static_cast<int>(cols); ++col) {
            ncplane_putchar_yx(host, y0 + row, col, ' ');
        }
    }

    draw_cover_thumb(host, y0);

    // Row 0: title.
    std::string title;
    std::string artist_album;
    std::string time_label = "0:00 / 0:00";
    double progress = 0.0;
    if (snap != nullptr && !snap->source.file_path.empty()) {
        const auto& t = snap->source.tags;
        title = t.title.empty() ? snap->source.file_path : t.title;
        if (!t.artist.empty() && !t.album.empty()) {
            artist_album = t.artist + " · " + t.album;
        } else if (!t.artist.empty()) {
            artist_album = t.artist;
        } else if (!t.album.empty()) {
            artist_album = t.album;
        }
        // Position: frames_written_at_track_start gives baseline within session.
        const auto rate = snap->source.sample_rate_hz;
        if (rate > 0 && snap->source.total_frames > 0) {
            const auto played_frames =
                snap->output.frames_written - snap->output.frames_written_at_track_start;
            const auto pos_ms = static_cast<long long>(
                played_frames * 1000ull / rate);
            const auto dur_ms = snap->source.duration.count();
            time_label = format_mm_ss(std::chrono::milliseconds(pos_ms))
                       + " / "
                       + format_mm_ss(std::chrono::milliseconds(dur_ms));
            if (dur_ms > 0) {
                progress = static_cast<double>(pos_ms)
                         / static_cast<double>(dur_ms);
            }
        }
    } else {
        title = std::string(kEm);
        artist_album = std::string(kEm);
    }

    set_default_fg(host);
    ncplane_putstr_yx(host, y0 + 0, info_x,
                      clip(title, static_cast<std::size_t>(info_w)).c_str());
    set_dim(host);
    ncplane_putstr_yx(host, y0 + 1, info_x,
                      clip(artist_album, static_cast<std::size_t>(info_w)).c_str());
    set_default_fg(host);

    // Row 2: seekbar  time  transport
    const std::string transport = playing
        ? std::string("⏮ ⏯ ⏭") // ⏮ ⏯ ⏭
        : std::string("⏮ ⏯ ⏭");
    const std::string& time_str = time_label;
    const int transport_cols = 7; // 3 glyphs + 2 spaces, generous slack
    const int time_cols = static_cast<int>(time_str.size());
    const int seekbar_min = 10;
    const int reserved = time_cols + transport_cols + 4; // gaps
    int seek_w = info_w - reserved;
    if (seek_w < seekbar_min) {
        seek_w = std::max(seekbar_min, info_w - time_cols - 2);
    }
    if (seek_w < 4) {
        seek_w = 4;
    }
    const int seek_x = info_x;
    render_seekbar(host, y0 + 2, seek_x, seek_w, progress);

    const int time_x = seek_x + seek_w + 1;
    if (time_x + time_cols <= static_cast<int>(cols)) {
        set_default_fg(host);
        ncplane_putstr_yx(host, y0 + 2, time_x, time_str.c_str());
    }
    const int trans_x = time_x + time_cols + 2;
    if (trans_x + transport_cols <= static_cast<int>(cols)) {
        set_default_fg(host);
        ncplane_putstr_yx(host, y0 + 2, trans_x, transport.c_str());
    }

    // Row 3: telemetry.
    std::string telem;
    if (snap != nullptr) {
        const auto* bp = bitperfect_label(snap->bit_perfect.level);
        std::string fmt = format_rate_bits_codec(
            snap->source.sample_rate_hz,
            snap->source.bit_depth_file,
            snap->source.codec_name);
        std::string dac = snap->device.fingerprint.alsa_card_name.empty()
            ? std::string(kEm)
            : snap->device.fingerprint.alsa_card_name;
        const auto* rtm = rt_mode_label(snap->realtime.status.mode);
        const double ring_frac = snap->ring.capacity_bytes > 0
            ? static_cast<double>(snap->ring.fill_bytes)
              / static_cast<double>(snap->ring.capacity_bytes)
            : 0.0;
        std::string ring = format_ring_fill(ring_frac);
        std::array<char, 16> xrun_buf{};
        std::snprintf(xrun_buf.data(), xrun_buf.size(),
                      "xrun %u", snap->output.xrun_count);
        telem = std::string(bp) + " · " + fmt + " · " + dac + " · "
              + rtm + " · " + ring + " · " + xrun_buf.data();
    } else {
        telem = std::string(kEm);
    }

    std::array<char, 16> vol_buf{};
    std::snprintf(vol_buf.data(), vol_buf.size(), " \U0001F50A %d%%",
                  std::clamp(volume_pct, 0, 100));
    // Telemetry on left, volume on right.
    set_dim(host);
    ncplane_putstr_yx(host, y0 + 3, info_x,
                      clip(telem, static_cast<std::size_t>(
                          info_w - static_cast<int>(std::strlen(vol_buf.data())) - 1))
                          .c_str());
    const int vol_x = static_cast<int>(cols)
                    - static_cast<int>(std::strlen(vol_buf.data()));
    if (vol_x > info_x) {
        ncplane_putstr_yx(host, y0 + 3, vol_x, vol_buf.data());
    }
    set_default_fg(host);
}

} // namespace transporter::tui::components
