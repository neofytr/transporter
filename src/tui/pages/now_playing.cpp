// SPDX-License-Identifier: GPL-3.0-or-later

#include "now_playing.hpp"

#include "draw_util.hpp"
#include "../components/cover.hpp"
#include "../components/player_bar.hpp"
#include "../components/seekbar.hpp"
#include "../components/transport.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/engine/telemetry.hpp>

#include <notcurses/notcurses.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

namespace transporter::tui::pages {

namespace {

struct Rgb { std::uint8_t r, g, b; };
constexpr Rgb kAccent{0x7A, 0xB8, 0xC8};
constexpr Rgb kDim   {0x90, 0x90, 0x90};
constexpr Rgb kFaint {0x50, 0x50, 0x50};

void set_rgb(struct ncplane* p, Rgb c) {
    ncplane_set_fg_rgb8(p, c.r, c.g, c.b);
}
void set_default(struct ncplane* p) { ncplane_set_fg_default(p); }

const char* bitperfect_label(engine::BitPerfectVerdict::Level lv) {
    using L = engine::BitPerfectVerdict::Level;
    switch (lv) {
    case L::Yes:       return "YES";
    case L::Qualified: return "QUAL";
    case L::No:        return "NO";
    }
    return "—";
}

// Compute position_ms relative to the current track from a snapshot.
std::int64_t snap_position_ms(const engine::PipelineSnapshot& s) {
    const auto rate = s.source.sample_rate_hz;
    if (rate == 0) {
        return 0;
    }
    const auto played =
        s.output.frames_written - s.output.frames_written_at_track_start;
    return static_cast<std::int64_t>(
        static_cast<std::uint64_t>(played) * 1000ull / rate);
}

void draw_cover_placeholder(struct ncplane* host, int y, int x, int w, int h) {
    set_rgb(host, kDim);
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            const char* g = " ";
            if (row == 0 && col == 0)              g = "\xE2\x95\x94"; // ╔
            else if (row == 0 && col == w - 1)     g = "\xE2\x95\x97"; // ╗
            else if (row == h - 1 && col == 0)     g = "\xE2\x95\x9A"; // ╚
            else if (row == h - 1 && col == w - 1) g = "\xE2\x95\x9D"; // ╝
            else if (row == 0 || row == h - 1)     g = "\xE2\x95\x90"; // ═
            else if (col == 0 || col == w - 1)     g = "\xE2\x95\x91"; // ║
            ncplane_putstr_yx(host, y + row, x + col, g);
        }
    }
    const int gy = y + h / 2;
    const int gx = x + w / 2;
    ncplane_putstr_yx(host, gy, gx, "\xE2\x99\xAB"); // ♫
    set_default(host);
}

void draw_empty_state(struct ncplane* host, int body_y0, int body_rows) {
    if (body_rows < 4) {
        return;
    }
    const int mid = body_y0 + body_rows / 2;
    set_rgb(host, kDim);
    put_centered(host, mid - 1, "\xE2\x99\xAB"); // ♫
    put_centered(host, mid + 1,
                 "No track loaded — open Library (1) and press Enter on a track");
    set_default(host);
}

} // namespace

NowPlayingPage::NowPlayingPage(engine::Engine* eng) : eng_(eng) {}

void NowPlayingPage::draw(struct ncplane* host, int body_y0, int body_rows,
                          const engine::PipelineSnapshot* snap) {
    draw_frame(host, body_y0, body_rows, "Now Playing");

    seek_y_ = seek_x_ = -1;
    seek_w_ = 0;
    transport_y_ = transport_x_ = -1;

    if (body_rows < 8) {
        return;
    }

    const bool have_track = snap != nullptr && !snap->source.file_path.empty();
    if (!have_track) {
        draw_empty_state(host, body_y0, body_rows);
        return;
    }

    unsigned hcols = 0, hrows = 0;
    ncplane_dim_yx(host, &hrows, &hcols);
    (void)hrows;
    const int width = static_cast<int>(hcols);
    if (width < 50) {
        draw_empty_state(host, body_y0, body_rows);
        return;
    }

    // Region inside the rounded frame, with padding.
    const int inner_x0 = 2;
    const int inner_y0 = body_y0 + 2;
    const int inner_w  = width - 2 - 2;
    const int inner_h  = body_rows - 1 - 2;

    // Cover ~40% width. Cells are roughly 2:1 tall:wide, so to keep the cover
    // visually square pick height ≈ width / 2.
    const int cover_w = std::clamp(inner_w * 40 / 100, 16, 36);
    const int cover_h = std::clamp(cover_w / 2, 6, inner_h - 1);

    const int cover_y = inner_y0;
    const int cover_x = inner_x0;

    // Try a real cover blit; fall back to placeholder.
    bool blitted = false;
    const std::filesystem::path tpath = snap->source.file_path;
    if (!tpath.empty()) {
        auto cov = cover_cache_.get_or_load(tpath,
            [&]{ return components::load_cover_for_track(tpath); });
        if (cov && *cov) {
            ncvisual* vis = components::visual_from_cover(*cov);
            if (vis != nullptr) {
                if (components::blit_into(host, vis, cover_y, cover_x,
                                          cover_h, cover_w) == 0) {
                    blitted = true;
                }
                ncvisual_destroy(vis);
            }
        }
    }
    if (!blitted) {
        draw_cover_placeholder(host, cover_y, cover_x, cover_w, cover_h);
    }
    last_path_ = tpath;

    // Info column.
    const int info_x = cover_x + cover_w + 3;
    const int info_w = inner_w - cover_w - 3 - 1;
    if (info_w < 12) {
        return;
    }
    const auto& tags = snap->source.tags;

    // Row 0: title (bold, white).
    ncplane_set_styles(host, NCSTYLE_BOLD);
    set_default(host);
    {
        std::string title = tags.title.empty()
            ? std::filesystem::path(snap->source.file_path).filename().string()
            : tags.title;
        ncplane_putstr_yx(host, inner_y0, info_x,
                          clip_with_ellipsis(title,
                              static_cast<std::size_t>(info_w)).c_str());
    }
    ncplane_set_styles(host, NCSTYLE_NONE);

    // Row 1: artist (dim).
    set_rgb(host, kDim);
    ncplane_putstr_yx(host, inner_y0 + 1, info_x,
                      clip_with_ellipsis(
                          tags.artist.empty() ? "—" : tags.artist,
                          static_cast<std::size_t>(info_w)).c_str());

    // Row 2: Album · YYYY. Year only when present and non-zero.
    {
        std::string year;
        if (tags.date.size() >= 4) {
            year = tags.date.substr(0, 4);
            // Reject "0000".
            bool all_zero = true;
            for (char c : year) {
                if (c != '0') { all_zero = false; break; }
            }
            if (all_zero) {
                year.clear();
            }
        }
        std::string line = tags.album.empty() ? std::string{"—"} : tags.album;
        if (!year.empty()) {
            line += " \xC2\xB7 ";  // ·
            line += year;
        }
        ncplane_putstr_yx(host, inner_y0 + 2, info_x,
                          clip_with_ellipsis(line,
                              static_cast<std::size_t>(info_w)).c_str());
    }

    // Row 3: format pill — "44.1k/16 FLAC · YES"
    {
        const std::string fmt = components::format_rate_bits_codec(
            snap->source.sample_rate_hz,
            snap->source.bit_depth_file,
            snap->source.codec_name);
        std::string line = fmt;
        line += " \xC2\xB7 ";  // ·
        line += bitperfect_label(snap->bit_perfect.level);
        ncplane_putstr_yx(host, inner_y0 + 3, info_x,
                          clip_with_ellipsis(line,
                              static_cast<std::size_t>(info_w)).c_str());
    }
    set_default(host);

    // Seekbar row (row 4 of the info column).
    const int seek_y = inner_y0 + 5;
    const int seek_x = info_x;
    const std::int64_t pos_ms = snap_position_ms(*snap);
    const std::int64_t dur_ms = snap->source.duration.count();
    const std::string time_label = components::format_time(pos_ms, dur_ms);
    const int time_cols = static_cast<int>(time_label.size());
    const int seek_gap = 2;
    int seek_w = info_w - time_cols - seek_gap;
    if (seek_w < 8) {
        seek_w = std::max(8, info_w - time_cols - 1);
    }
    if (seek_w < 4) {
        return;
    }
    components::SeekbarStyle style;
    components::draw_seekbar(host, seek_y, seek_x, seek_w,
                             pos_ms, dur_ms,
                             /*waveform_height=*/1, style);
    // Time label to the right of the bar.
    set_default(host);
    const int time_x = seek_x + seek_w + seek_gap;
    if (time_x + time_cols <= info_x + info_w + 1) {
        ncplane_putstr_yx(host, seek_y, time_x, time_label.c_str());
    }
    seek_y_ = seek_y;
    seek_x_ = seek_x;
    seek_w_ = seek_w;

    // Reserved viz rows below the waveform (4 rows spectrum + 2 rows VU).
    // Render a faint placeholder so the layout doesn't reflow when T6 lands.
    const int viz_y0 = seek_y + 2;          // +1 waveform, +1 spacer
    const int viz_rows = 6;                 // 4 spectrum + 2 VU
    if (viz_y0 + viz_rows < body_y0 + body_rows - 1) {
        set_rgb(host, kFaint);
        for (int r = 0; r < viz_rows; ++r) {
            const int y = viz_y0 + r;
            // " · · · · · · · " pattern.
            int wrote = 0;
            for (int i = 0; i < info_w; i += 2) {
                ncplane_putstr_yx(host, y, info_x + i, "\xC2\xB7"); // ·
                ++wrote;
                if (wrote * 2 >= info_w) {
                    break;
                }
            }
        }
        set_default(host);
    }

    // Transport row, centred under the info column.
    const int transport_y = std::min(viz_y0 + viz_rows + 1,
                                     body_y0 + body_rows - 2);
    if (transport_y > seek_y + 1 && transport_y < body_y0 + body_rows - 1) {
        const int transport_x = info_x
            + std::max(0, (info_w - components::kTransportWidth) / 2);
        components::draw_transport(host, transport_y, transport_x);
        transport_y_ = transport_y;
        transport_x_ = transport_x;
    }
}

bool NowPlayingPage::handle_mouse(int y, int x,
                                  const engine::PipelineSnapshot* snap) {
    if (eng_ == nullptr) {
        return false;
    }
    // Seekbar click: drag-to-seek for v1 is the same as click-to-seek.
    if (y == seek_y_ && x >= seek_x_ && x < seek_x_ + seek_w_) {
        if (snap == nullptr || snap->source.sample_rate_hz == 0) {
            return false;
        }
        const double ratio =
            components::position_ratio(x - seek_x_, seek_w_);
        const std::uint64_t target_frame =
            static_cast<std::uint64_t>(
                ratio * static_cast<double>(snap->source.total_frames));
        (void)eng_->seek(target_frame);
        return true;
    }
    // Transport click zones.
    if (y == transport_y_ && transport_x_ >= 0) {
        const int rel = x - transport_x_;
        const auto hit = components::hit_test_transport(rel);
        if (hit == components::TransportHit::PlayPause) {
            if (eng_->state() == engine::State::Playing) {
                (void)eng_->pause();
            } else {
                (void)eng_->play();
            }
            return true;
        }
        if (hit == components::TransportHit::Prev
            || hit == components::TransportHit::Next) {
            // Queue manipulation lives at the App layer; signal via the
            // caller so it can advance/retreat.  Return false so the App
            // dispatcher picks it up. For T3 this stays a no-op here;
            // the App handles 'n' / ',' via the same routine.
            return false;
        }
    }
    return false;
}

void NowPlayingPage::seek_relative(std::int64_t delta_ms,
                                   const engine::PipelineSnapshot* snap) {
    if (eng_ == nullptr || snap == nullptr) {
        return;
    }
    const auto rate = snap->source.sample_rate_hz;
    if (rate == 0) {
        return;
    }
    const std::int64_t cur = snap_position_ms(*snap);
    std::int64_t target_ms = cur + delta_ms;
    if (target_ms < 0) {
        target_ms = 0;
    }
    const std::int64_t dur_ms = snap->source.duration.count();
    if (dur_ms > 0 && target_ms > dur_ms) {
        target_ms = dur_ms;
    }
    const std::uint64_t target_frame =
        static_cast<std::uint64_t>(target_ms) * rate / 1000ull;
    (void)eng_->seek(target_frame);
}

} // namespace transporter::tui::pages
