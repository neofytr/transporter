// SPDX-License-Identifier: GPL-3.0-or-later
#include "playlist.hpp"

#include <fstream>
#include <string>
#include <system_error>

namespace transporter::gui {

std::vector<std::filesystem::path>
load_playlist(const std::filesystem::path& playlist_path)
{
    std::ifstream f(playlist_path);
    if (!f.is_open()) {
        return {};
    }

    const auto base = playlist_path.parent_path();
    std::vector<std::filesystem::path> result;
    std::string line;

    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }

        std::filesystem::path p{line};
        if (p.is_relative()) {
            p = base / p;
        }

        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) {
            continue;
        }

        result.push_back(std::move(p));
    }

    return result;
}

} // namespace transporter::gui
