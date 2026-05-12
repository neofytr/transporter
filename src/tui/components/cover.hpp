// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_TUI_COMPONENTS_COVER_HPP
#define TRANSPORTER_TUI_COMPONENTS_COVER_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

struct ncplane;
struct ncvisual;

namespace transporter::tui::components {

// Decoded RGBA pixel buffer. R in the low byte (memory order R, G, B, A).
// Row-major, width*height pixels. Decoded once per file; reused for blits.
struct Cover {
    std::unique_ptr<std::uint32_t[]> pixels;
    int width  = 0;
    int height = 0;
    explicit operator bool() const { return pixels && width > 0 && height > 0; }
};

// Decode the file at `path` (JPEG or PNG). Returns an empty Cover on failure
// (unknown extension, decoder error, missing file, etc.).
Cover decode_image(const std::filesystem::path& path);

// Decode an embedded picture from a raw byte buffer (e.g. an APIC frame, a
// FLAC PICTURE block payload). Returns empty Cover on failure.
Cover decode_embedded(const std::vector<unsigned char>& bytes);

// Find the cover for a track. Strategy (Picard / Wikipedia conventions):
//   1. Embedded picture extraction — placeholder; wired in a later phase
//      when decoder Tags expose a picture payload.
//   2. Sidecar files in the track's directory, case-insensitive on filename:
//        cover.{jpg,jpeg,png}
//        folder.{jpg,jpeg,png}
//        front.{jpg,jpeg,png}
//        albumart*.{jpg,jpeg,png}
//   3. Empty Cover otherwise.
Cover load_cover_for_track(const std::filesystem::path& track_path);

// Build an ncvisual from a Cover. The visual takes its own copy of the pixel
// data; the Cover can be freed after this call. Returns nullptr if cov is
// empty or notcurses rejects the input.
ncvisual* visual_from_cover(const Cover& cov);

// Blitter dispatch — pick the best supported blitter for the current terminal.
//   - kitty graphics or sixel  -> NCBLIT_PIXEL
//   - truecolor without pixel  -> NCBLIT_2x2 (quadrants)
//   - everything else          -> NCBLIT_DEFAULT (halfblock)
// Inside tmux, kitty-graphics is suppressed in favour of sixel.
struct BlitChoice {
    int  blitter = 0;     // ncblitter_e value
    bool pixel   = false; // true when the chosen blitter is NCBLIT_PIXEL
};

BlitChoice pick_blitter();

// Blit `vis` into `dst` at the cell rect (y, x, cells_h, cells_w). Returns 0
// on success, non-zero on failure. Stretch-scaled; uses the blitter chosen
// by pick_blitter().
int blit_into(struct ncplane* dst, struct ncvisual* vis,
              int y, int x, int cells_h, int cells_w);

} // namespace transporter::tui::components

#endif
