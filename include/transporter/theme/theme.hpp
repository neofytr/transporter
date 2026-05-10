// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TRANSPORTER_THEME_THEME_HPP
#define TRANSPORTER_THEME_THEME_HPP

#include <filesystem>

namespace transporter::theme {

struct Color4 { float r, g, b, a; };

struct Theme {
    Color4 accent      = {1.00f, 0.60f, 0.00f, 1.0f}; // amber default
    Color4 accent2     = {0.36f, 0.83f, 0.92f, 1.0f}; // cyan default
    Color4 border_dim  = {0.13f, 0.13f, 0.13f, 1.0f}; // inactive border default
    float  rounding    = 0.0f;
    float  border_size = 1.0f;

    // Load from ~/.config/hypr/hyprland.conf (follows source= includes).
    // Falls back to defaults on missing file or parse error.
    static Theme load(const std::filesystem::path& hypr_conf);
    static Theme defaults();
};

// Apply t to the current ImGui context's style + color table.
// Must be called after ImGui::CreateContext().
void apply_to_imgui(const Theme& t);

} // namespace transporter::theme

#endif
