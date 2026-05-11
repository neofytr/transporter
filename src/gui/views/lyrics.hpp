// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace transporter::gui {

struct LyricLine {
    std::int64_t time_ms;  // onset, milliseconds
    std::string text;
};

struct Lyrics {
    std::vector<LyricLine> lines;  // sorted by time_ms
    bool loaded = false;
};

// Find and parse an LRC file adjacent to `audio_path`.
// Probes: <stem>.lrc, <stem>.LRC, cover.lrc — in that order.
// Returns a Lyrics with loaded=false if none found.
Lyrics load_lyrics(const std::filesystem::path& audio_path);

// Return the index of the current line for `elapsed_ms`.
// Returns -1 if before the first line.
int current_lyric_line(const Lyrics& lyr, std::int64_t elapsed_ms);

} // namespace transporter::gui
