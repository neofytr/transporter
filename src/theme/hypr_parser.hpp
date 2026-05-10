// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_THEME_HYPR_PARSER_HPP
#define TRANSPORTER_THEME_HYPR_PARSER_HPP

#include <filesystem>
#include <string>
#include <unordered_map>

namespace transporter::theme::detail {

// Parse hyprland.conf into a flat map of "section.key" -> "raw value string".
// Follows source= includes (expand ~, max depth 8).
// Substitutes $variable references.
std::unordered_map<std::string, std::string>
parse_hypr_flat(const std::filesystem::path& config_file);

} // namespace transporter::theme::detail

#endif
