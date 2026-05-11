// SPDX-License-Identifier: GPL-3.0-or-later

#include "lyrics.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

namespace transporter::gui {

Lyrics load_lyrics(const std::filesystem::path& audio_path) {
    const auto dir  = audio_path.parent_path();
    const auto stem = audio_path.stem();

    const std::filesystem::path candidates[] = {
        dir / (stem.string() + ".lrc"),
        dir / (stem.string() + ".LRC"),
    };

    std::ifstream f;
    for (const auto& p : candidates) {
        f.open(p);
        if (f.is_open()) break;
    }
    if (!f.is_open()) return {};

    Lyrics out;
    std::string line;
    while (std::getline(f, line)) {
        int mm = 0, ss = 0, cs = 0;
        if (std::sscanf(line.c_str(), "[%d:%d.%d]", &mm, &ss, &cs) != 3)
            continue;

        // Advance past the tag to the lyric text.
        const auto close = line.find(']');
        if (close == std::string::npos) continue;
        std::string text = line.substr(close + 1);

        // Skip metadata tags — their text contains ':' (e.g. "ti:", "ar:").
        if (text.find(':') != std::string::npos) continue;

        out.lines.push_back({
            static_cast<std::int64_t>((mm * 60 + ss) * 1000 + cs * 10),
            std::move(text),
        });
    }

    std::stable_sort(out.lines.begin(), out.lines.end(),
                     [](const LyricLine& a, const LyricLine& b) {
                         return a.time_ms < b.time_ms;
                     });

    out.loaded = !out.lines.empty();
    return out;
}

int current_lyric_line(const Lyrics& lyr, std::int64_t elapsed_ms) {
    if (lyr.lines.empty()) return -1;

    // Last line whose onset <= elapsed.
    auto it = std::upper_bound(lyr.lines.begin(), lyr.lines.end(), elapsed_ms,
                               [](std::int64_t ms, const LyricLine& l) {
                                   return ms < l.time_ms;
                               });
    if (it == lyr.lines.begin()) return -1;
    --it;
    return static_cast<int>(it - lyr.lines.begin());
}

} // namespace transporter::gui
