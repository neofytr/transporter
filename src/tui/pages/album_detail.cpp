// SPDX-License-Identifier: GPL-3.0-or-later

#include "album_detail.hpp"

#include "draw_util.hpp"
#include "../components/player_bar.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/library/library.hpp>

#include <notcurses/notcurses.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <string_view>

namespace transporter::tui::pages {

namespace {

constexpr int kHeaderRows  = 7;   // cover hero + meta header
constexpr int kCoverWidth  = 12;
constexpr int kCoverHeight = 6;
constexpr int kColGap      = 2;

struct Rgb { std::uint8_t r, g, b; };
constexpr Rgb kAccent{0x7A, 0xB8, 0xC8};
constexpr Rgb kDim   {0x90, 0x90, 0x90};
constexpr Rgb kSelBg {0x2A, 0x40, 0x4A};

void set_rgb(struct ncplane* p, Rgb c) {
    ncplane_set_fg_rgb8(p, c.r, c.g, c.b);
}

void set_default(struct ncplane* p) {
    ncplane_set_fg_default(p);
}

std::string format_track_no(const library::Track& t) {
    if (t.track_no.empty()) {
        return "  ";
    }
    // Strip a trailing /N suffix when present ("3/12" -> "3").
    std::string s = t.track_no;
    const auto slash = s.find('/');
    if (slash != std::string::npos) {
        s.resize(slash);
    }
    return s;
}

int parse_disc(const std::string& d) {
    if (d.empty()) {
        return 1;
    }
    int n = 0;
    for (char c : d) {
        if (c == '/') {
            break;
        }
        if (c >= '0' && c <= '9') {
            n = n * 10 + (c - '0');
        } else {
            return n > 0 ? n : 1;
        }
    }
    return n > 0 ? n : 1;
}

int count_discs(const std::vector<library::Track>& tracks) {
    std::set<int> discs;
    for (const library::Track& t : tracks) {
        discs.insert(parse_disc(t.disc_no));
    }
    return static_cast<int>(discs.size());
}

std::string total_duration_label(const std::vector<library::Track>& tracks) {
    std::chrono::milliseconds sum{0};
    for (const library::Track& t : tracks) {
        sum += t.duration;
    }
    return components::format_mm_ss(sum);
}

// Returns a format pill spread for the album, like "44.1k/16 FLAC" when
// homogeneous, or "mixed 44.1k–96k FLAC" when rates differ.
std::string format_spread(const std::vector<library::Track>& tracks) {
    if (tracks.empty()) {
        return "—";
    }
    std::uint32_t min_rate = tracks.front().sample_rate_hz;
    std::uint32_t max_rate = tracks.front().sample_rate_hz;
    std::uint16_t bits = tracks.front().bit_depth;
    bool same_bits = true;
    std::string codec = tracks.front().codec;
    bool same_codec = true;
    for (const library::Track& t : tracks) {
        min_rate = std::min(min_rate, t.sample_rate_hz);
        max_rate = std::max(max_rate, t.sample_rate_hz);
        if (t.bit_depth != bits) {
            same_bits = false;
        }
        if (t.codec != codec) {
            same_codec = false;
        }
    }
    if (min_rate == max_rate && same_bits && same_codec) {
        return components::format_rate_bits_codec(min_rate, bits, codec);
    }
    std::array<char, 64> buf{};
    const double kmin = static_cast<double>(min_rate) / 1000.0;
    const double kmax = static_cast<double>(max_rate) / 1000.0;
    std::snprintf(buf.data(), buf.size(), "mixed %.1fk–%.1fk %s",
                  kmin, kmax,
                  same_codec ? codec.c_str() : "varied");
    return std::string(buf.data());
}

void draw_cover_placeholder(struct ncplane* host, int y, int x, int w, int h) {
    set_rgb(host, kDim);
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            if (row == 0 && col == 0) {
                ncplane_putstr_yx(host, y + row, x + col, "╔");
            } else if (row == 0 && col == w - 1) {
                ncplane_putstr_yx(host, y + row, x + col, "╗");
            } else if (row == h - 1 && col == 0) {
                ncplane_putstr_yx(host, y + row, x + col, "╚");
            } else if (row == h - 1 && col == w - 1) {
                ncplane_putstr_yx(host, y + row, x + col, "╝");
            } else if (row == 0 || row == h - 1) {
                ncplane_putstr_yx(host, y + row, x + col, "═");
            } else if (col == 0 || col == w - 1) {
                ncplane_putstr_yx(host, y + row, x + col, "║");
            } else {
                ncplane_putchar_yx(host, y + row, x + col, ' ');
            }
        }
    }
    const int gy = y + h / 2;
    const int gx = x + w / 2;
    ncplane_putstr_yx(host, gy, gx, "♫");
    set_default(host);
}

} // namespace

AlbumDetailPage::AlbumDetailPage(library::Library* lib, engine::Engine* eng)
    : lib_(lib), eng_(eng) {}

void AlbumDetailPage::set_album(std::optional<std::int64_t> album_id) {
    album_id_ = album_id;
    tracks_.clear();
    selected_ = 0;
    scroll_ = 0;
    title_.clear();
    album_artist_.clear();
    date_.clear();
    if (album_id_.has_value()) {
        reload();
    }
}

void AlbumDetailPage::reload() {
    if (lib_ == nullptr || !album_id_.has_value()) {
        return;
    }
    auto r = lib_->tracks_in_album(album_id_.value());
    if (r.has_value()) {
        tracks_ = std::move(r.value());
    }
    if (!tracks_.empty()) {
        // Pull album-level fields from the first track.
        title_ = tracks_.front().album;
        album_artist_ = tracks_.front().album_artist.empty()
                      ? tracks_.front().artist
                      : tracks_.front().album_artist;
        date_ = tracks_.front().date;
    }
    clamp_selection();
}

void AlbumDetailPage::clamp_selection() {
    if (tracks_.empty()) {
        selected_ = 0;
        scroll_ = 0;
        return;
    }
    if (selected_ >= tracks_.size()) {
        selected_ = tracks_.size() - 1;
    }
}

void AlbumDetailPage::draw(struct ncplane* host, int body_y0, int body_rows) {
    // Build the frame title: "Artist — Album" when both present.
    std::string frame_title;
    if (!album_artist_.empty() && !title_.empty()) {
        frame_title = album_artist_ + " — " + title_;
    } else if (!title_.empty()) {
        frame_title = title_;
    } else {
        frame_title = "Album";
    }
    draw_frame(host, body_y0, body_rows, frame_title);

    if (body_rows < kHeaderRows + 3 || tracks_.empty()) {
        return;
    }

    unsigned hcols = 0, hrows = 0;
    ncplane_dim_yx(host, &hrows, &hcols);
    (void)hrows;
    const int width = static_cast<int>(hcols);
    if (width < 50) {
        return;
    }

    // Header region: cover at left, meta to its right.
    const int hdr_x0 = 2;
    const int hdr_y0 = body_y0 + 2;
    draw_cover_placeholder(host, hdr_y0, hdr_x0, kCoverWidth, kCoverHeight);

    const int meta_x = hdr_x0 + kCoverWidth + kColGap;
    const int meta_w = width - meta_x - 2;
    if (meta_w <= 4) {
        return;
    }

    // Meta row 0: title (bold).
    ncplane_set_styles(host, NCSTYLE_BOLD);
    set_default(host);
    const std::string title_clip = clip_with_ellipsis(
        title_, static_cast<std::size_t>(meta_w));
    ncplane_putstr_yx(host, hdr_y0, meta_x, title_clip.c_str());
    ncplane_set_styles(host, NCSTYLE_NONE);

    // Meta row 1: artist · year · N disc · MM:SS.
    const int disc_count = count_discs(tracks_);
    std::array<char, 64> subline{};
    std::string year = date_.size() >= 4 ? date_.substr(0, 4) : date_;
    std::snprintf(subline.data(), subline.size(),
                  "%s · %s · %d disc · %s",
                  album_artist_.c_str(),
                  year.empty() ? "—" : year.c_str(),
                  disc_count,
                  total_duration_label(tracks_).c_str());
    const std::string sub = clip_with_ellipsis(subline.data(),
        static_cast<std::size_t>(meta_w));
    set_rgb(host, kDim);
    ncplane_putstr_yx(host, hdr_y0 + 1, meta_x, sub.c_str());

    // Meta row 2: format spread.
    const std::string spread = clip_with_ellipsis(format_spread(tracks_),
        static_cast<std::size_t>(meta_w));
    ncplane_putstr_yx(host, hdr_y0 + 2, meta_x, spread.c_str());
    set_default(host);

    // Track list region.
    const int list_y0 = body_y0 + kHeaderRows + 2;
    const int list_rows_avail = body_y0 + body_rows - 1 - list_y0;
    if (list_rows_avail < 2) {
        return;
    }
    last_visible_ = list_rows_avail;

    // Adjust scroll to keep selection in view.
    if (selected_ < scroll_) {
        scroll_ = selected_;
    } else if (selected_ >= scroll_ + static_cast<std::size_t>(list_rows_avail)) {
        scroll_ = selected_ - static_cast<std::size_t>(list_rows_avail) + 1;
    }

    const bool multi_disc = disc_count > 1;
    int row_y = list_y0;
    int last_disc = -1;
    for (std::size_t i = scroll_; i < tracks_.size(); ++i) {
        if (row_y >= body_y0 + body_rows - 1) {
            break;
        }
        const library::Track& t = tracks_[i];
        const int disc = parse_disc(t.disc_no);
        // Disc separator (only for the first visible track of a new disc).
        if (multi_disc && disc != last_disc) {
            if (row_y + 1 >= body_y0 + body_rows - 1) {
                break;
            }
            set_rgb(host, kDim);
            std::array<char, 32> sep{};
            std::snprintf(sep.data(), sep.size(), "── Disc %d ──", disc);
            ncplane_putstr_yx(host, row_y, 2, sep.data());
            set_default(host);
            ++row_y;
            last_disc = disc;
        } else if (last_disc < 0) {
            last_disc = disc;
        }

        const bool selected = (i == selected_);
        if (selected) {
            ncplane_set_bg_rgb8(host, kSelBg.r, kSelBg.g, kSelBg.b);
            set_rgb(host, kAccent);
        } else {
            set_default(host);
        }

        // Compose the row. Layout (left-to-right):
        //   "  NN  Title ............................... MM:SS  44.1k/16 FLAC"
        const std::string tno = format_track_no(t);
        const std::string dur = components::format_mm_ss(t.duration);
        const std::string fmt = components::format_rate_bits_codec(
            t.sample_rate_hz, t.bit_depth, t.codec);

        // Layout widths (approximate; truncate title to fit).
        const int row_w = width - 4;
        const int right_dur_w = static_cast<int>(dur.size());
        const int right_fmt_w = static_cast<int>(fmt.size());
        const int right_gap = 2;
        const int right_total = right_dur_w + right_gap + right_fmt_w;
        const int left_w = 6;  // "  NN  " incl. two-cell number
        const int title_w = std::max(8, row_w - left_w - right_total - 4);

        std::array<char, 16> tno_buf{};
        std::snprintf(tno_buf.data(), tno_buf.size(), "%2s",
                      tno.empty() ? "" : tno.c_str());

        // Clear background of full row when selected.
        if (selected) {
            for (int x = 0; x < width; ++x) {
                ncplane_putchar_yx(host, row_y, x, ' ');
            }
        }
        ncplane_putstr_yx(host, row_y, 2, tno_buf.data());
        const std::string title_clipped = clip_with_ellipsis(t.title,
            static_cast<std::size_t>(title_w));
        ncplane_putstr_yx(host, row_y, 2 + left_w, title_clipped.c_str());
        const int dur_x = width - 2 - right_total;
        ncplane_putstr_yx(host, row_y, dur_x, dur.c_str());
        ncplane_putstr_yx(host, row_y, dur_x + right_dur_w + right_gap,
                          fmt.c_str());

        if (selected) {
            ncplane_set_bg_default(host);
            set_default(host);
        }
        ++row_y;
    }
}

bool AlbumDetailPage::move_up(int count) {
    if (tracks_.empty()) {
        return false;
    }
    const int n = std::max(1, count);
    std::size_t s = selected_;
    for (int i = 0; i < n && s > 0; ++i) {
        --s;
    }
    if (s == selected_) {
        return false;
    }
    selected_ = s;
    return true;
}

bool AlbumDetailPage::move_down(int count) {
    if (tracks_.empty()) {
        return false;
    }
    const int n = std::max(1, count);
    std::size_t s = selected_;
    for (int i = 0; i < n && s + 1 < tracks_.size(); ++i) {
        ++s;
    }
    if (s == selected_) {
        return false;
    }
    selected_ = s;
    return true;
}

bool AlbumDetailPage::goto_top() {
    if (tracks_.empty() || selected_ == 0) {
        return false;
    }
    selected_ = 0;
    return true;
}

bool AlbumDetailPage::goto_bottom() {
    if (tracks_.empty() || selected_ + 1 == tracks_.size()) {
        return false;
    }
    selected_ = tracks_.size() - 1;
    return true;
}

bool AlbumDetailPage::half_page_down() {
    return move_down(std::max(1, last_visible_ / 2));
}

bool AlbumDetailPage::half_page_up() {
    return move_up(std::max(1, last_visible_ / 2));
}

bool AlbumDetailPage::play_selected() {
    if (tracks_.empty() || eng_ == nullptr) {
        return false;
    }
    if (selected_ >= tracks_.size()) {
        return false;
    }
    const library::Track& t = tracks_[selected_];
    // Locked Enter semantics: replace queue + play now.
    auto lr = eng_->load(t.path);
    if (!lr.has_value()) {
        return false;
    }
    auto pr = eng_->play();
    return pr.has_value();
}

bool AlbumDetailPage::append_selected() {
    // Queue stub — real queue plumbing lands in T4.
    return !tracks_.empty() && selected_ < tracks_.size();
}

bool AlbumDetailPage::play_selected_next() {
    return !tracks_.empty() && selected_ < tracks_.size();
}

} // namespace transporter::tui::pages
