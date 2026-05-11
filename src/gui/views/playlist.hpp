// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <vector>

namespace transporter::gui {

// Parse an M3U or M3U8 playlist. Relative paths are resolved against the
// playlist's parent directory. Non-existent paths are silently skipped.
// Returns an empty vector on I/O error or if the file has no valid entries.
std::vector<std::filesystem::path>
load_playlist(const std::filesystem::path& playlist_path);

} // namespace transporter::gui
