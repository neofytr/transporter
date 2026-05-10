// SPDX-License-Identifier: GPL-3.0-or-later

#include <transporter/theme/theme.hpp>
#include "hypr_parser.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace transporter::theme {

namespace {

// Parse a single color token. Recognises:
//   rgba(rrggbbaa)  — 8 hex chars
//   rgb(rrggbb)     — 6 hex chars
//   0xAARRGGBB      — 0x-prefixed, byte order: AA RR GG BB
// Returns nullopt on failure.
std::optional<Color4> parse_color_token(const std::string& tok) {
    if (tok.size() >= 5 && tok.substr(0, 4) == "rgba") {
        // rgba(rrggbbaa)
        const std::size_t lp = tok.find('(');
        const std::size_t rp = tok.find(')');
        if (lp == std::string::npos || rp == std::string::npos || rp < lp + 8) {
            return std::nullopt;
        }
        const std::string hex = tok.substr(lp + 1, rp - lp - 1);
        if (hex.size() < 8) return std::nullopt;
        try {
            const std::uint32_t rr = std::stoul(hex.substr(0, 2), nullptr, 16);
            const std::uint32_t gg = std::stoul(hex.substr(2, 2), nullptr, 16);
            const std::uint32_t bb = std::stoul(hex.substr(4, 2), nullptr, 16);
            const std::uint32_t aa = std::stoul(hex.substr(6, 2), nullptr, 16);
            return Color4{
                static_cast<float>(rr) / 255.0f,
                static_cast<float>(gg) / 255.0f,
                static_cast<float>(bb) / 255.0f,
                static_cast<float>(aa) / 255.0f,
            };
        } catch (...) { return std::nullopt; }
    }

    if (tok.size() >= 4 && tok.substr(0, 3) == "rgb") {
        const std::size_t lp = tok.find('(');
        const std::size_t rp = tok.find(')');
        if (lp == std::string::npos || rp == std::string::npos || rp < lp + 6) {
            return std::nullopt;
        }
        const std::string hex = tok.substr(lp + 1, rp - lp - 1);
        if (hex.size() < 6) return std::nullopt;
        try {
            const std::uint32_t rr = std::stoul(hex.substr(0, 2), nullptr, 16);
            const std::uint32_t gg = std::stoul(hex.substr(2, 2), nullptr, 16);
            const std::uint32_t bb = std::stoul(hex.substr(4, 2), nullptr, 16);
            return Color4{
                static_cast<float>(rr) / 255.0f,
                static_cast<float>(gg) / 255.0f,
                static_cast<float>(bb) / 255.0f,
                1.0f,
            };
        } catch (...) { return std::nullopt; }
    }

    if (tok.size() >= 3 && tok.substr(0, 2) == "0x") {
        // 0xAARRGGBB
        try {
            const std::uint32_t val = static_cast<std::uint32_t>(
                std::stoul(tok.substr(2), nullptr, 16));
            const std::uint32_t aa = (val >> 24) & 0xffu;
            const std::uint32_t rr = (val >> 16) & 0xffu;
            const std::uint32_t gg = (val >>  8) & 0xffu;
            const std::uint32_t bb = (val      ) & 0xffu;
            return Color4{
                static_cast<float>(rr) / 255.0f,
                static_cast<float>(gg) / 255.0f,
                static_cast<float>(bb) / 255.0f,
                static_cast<float>(aa) / 255.0f,
            };
        } catch (...) { return std::nullopt; }
    }

    return std::nullopt;
}

// Split raw value on spaces, try parsing each token as a color.
// Returns up to two colors (first two successful parses).
std::vector<Color4> parse_colors(const std::string& raw) {
    std::vector<Color4> result;
    std::string tok;
    for (std::size_t i = 0; i <= raw.size(); ++i) {
        const char c = (i < raw.size()) ? raw[i] : ' ';
        if (c == ' ' || c == '\t') {
            if (!tok.empty()) {
                if (auto col = parse_color_token(tok); col) {
                    result.push_back(*col);
                    if (result.size() == 2) break;
                }
                tok.clear();
            }
        } else {
            tok += c;
        }
    }
    return result;
}

float safe_stof(const std::string& s) {
    try { return std::stof(s); } catch (...) { return 0.0f; }
}

} // namespace

Theme Theme::defaults() {
    return Theme{};
}

Theme Theme::load(const std::filesystem::path& hypr_conf) {
    Theme t = defaults();
    const auto map = detail::parse_hypr_flat(hypr_conf);
    if (map.empty()) return t;

    auto get = [&](const std::string& key) -> const std::string* {
        auto it = map.find(key);
        if (it == map.end()) return nullptr;
        return &it->second;
    };

    if (const std::string* v = get("general.col.active_border")) {
        const auto cols = parse_colors(*v);
        if (cols.size() >= 1) t.accent  = cols[0];
        if (cols.size() >= 2) t.accent2 = cols[1];
    }

    if (const std::string* v = get("general.col.inactive_border")) {
        const auto cols = parse_colors(*v);
        if (!cols.empty()) t.border_dim = cols[0];
    }

    if (const std::string* v = get("decoration.rounding")) {
        t.rounding = safe_stof(*v);
    }

    if (const std::string* v = get("general.border_size")) {
        t.border_size = safe_stof(*v);
        if (t.border_size < 1.0f) t.border_size = 1.0f;
    }

    return t;
}

void apply_to_imgui(const Theme& t) {
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding    = t.rounding;
    style.FrameRounding     = t.rounding;
    style.ChildRounding     = t.rounding;
    style.PopupRounding     = t.rounding;
    style.ScrollbarRounding = t.rounding;
    style.GrabRounding      = t.rounding;
    style.TabRounding       = t.rounding;
    style.WindowBorderSize  = t.border_size;
    style.FrameBorderSize   = 0.0f;

    // Palette derived from accent colors.
    const ImVec4 bg     {0.05f, 0.05f, 0.07f, 1.0f};
    const ImVec4 bg_med {0.10f, 0.10f, 0.13f, 1.0f};
    const ImVec4 bg_hi  {0.16f, 0.16f, 0.20f, 1.0f};
    const ImVec4 text   {0.90f, 0.90f, 0.93f, 1.0f};
    const ImVec4 text_d {0.50f, 0.50f, 0.53f, 1.0f};

    const float ar = t.accent.r, ag = t.accent.g, ab = t.accent.b;
    const float br = t.border_dim.r, bg2 = t.border_dim.g, bb = t.border_dim.b;
    const float a2r = t.accent2.r, a2g = t.accent2.g, a2b = t.accent2.b;

    const ImVec4 acc    {ar,  ag,  ab,  1.0f};
    const ImVec4 acc_dim{ar * 0.40f, ag * 0.40f, ab * 0.40f, 0.70f};
    const ImVec4 acc_hi {std::min(ar * 1.25f, 1.0f),
                         std::min(ag * 1.25f, 1.0f),
                         std::min(ab * 1.25f, 1.0f), 1.0f};
    const ImVec4 border {br, bg2, bb, 0.60f};
    const ImVec4 acc2   {a2r, a2g, a2b, 1.0f};

    ImVec4* c = style.Colors;

    c[ImGuiCol_Text]                 = text;
    c[ImGuiCol_TextDisabled]         = text_d;
    c[ImGuiCol_WindowBg]             = bg;
    c[ImGuiCol_ChildBg]              = bg_med;
    c[ImGuiCol_PopupBg]              = bg_med;
    c[ImGuiCol_Border]               = border;
    c[ImGuiCol_BorderShadow]         = {0.0f, 0.0f, 0.0f, 0.0f};
    c[ImGuiCol_FrameBg]              = bg_hi;
    c[ImGuiCol_FrameBgHovered]       = acc_dim;
    c[ImGuiCol_FrameBgActive]        = {ar, ag, ab, 0.70f};
    c[ImGuiCol_TitleBg]              = bg;
    c[ImGuiCol_TitleBgActive]        = bg_med;
    c[ImGuiCol_TitleBgCollapsed]     = bg;
    c[ImGuiCol_MenuBarBg]            = bg_med;
    c[ImGuiCol_ScrollbarBg]          = bg;
    c[ImGuiCol_ScrollbarGrab]        = bg_hi;
    c[ImGuiCol_ScrollbarGrabHovered] = acc_dim;
    c[ImGuiCol_ScrollbarGrabActive]  = acc;
    c[ImGuiCol_CheckMark]            = acc;
    c[ImGuiCol_SliderGrab]           = acc;
    c[ImGuiCol_SliderGrabActive]     = acc_hi;
    c[ImGuiCol_Button]               = bg_hi;
    c[ImGuiCol_ButtonHovered]        = acc_dim;
    c[ImGuiCol_ButtonActive]         = {ar, ag, ab, 0.90f};
    c[ImGuiCol_Header]               = acc_dim;
    c[ImGuiCol_HeaderHovered]        = {ar * 0.70f, ag * 0.70f, ab * 0.70f, 0.80f};
    c[ImGuiCol_HeaderActive]         = acc;
    c[ImGuiCol_Separator]            = bg_hi;
    c[ImGuiCol_SeparatorHovered]     = acc_dim;
    c[ImGuiCol_SeparatorActive]      = acc;
    c[ImGuiCol_ResizeGrip]           = {0.0f, 0.0f, 0.0f, 0.0f};
    c[ImGuiCol_ResizeGripHovered]    = acc_dim;
    c[ImGuiCol_ResizeGripActive]     = acc;
    c[ImGuiCol_Tab]                  = bg_med;
    c[ImGuiCol_TabHovered]           = acc_dim;
    c[ImGuiCol_TabActive]            = {ar * 0.55f, ag * 0.55f, ab * 0.55f, 1.0f};
    c[ImGuiCol_TabUnfocused]         = bg_med;
    c[ImGuiCol_TabUnfocusedActive]   = bg_hi;
    c[ImGuiCol_PlotLines]            = acc2;
    c[ImGuiCol_PlotLinesHovered]     = acc2;
    c[ImGuiCol_PlotHistogram]        = acc2;
    c[ImGuiCol_PlotHistogramHovered] = acc2;
    c[ImGuiCol_TableHeaderBg]        = bg_med;
    c[ImGuiCol_TableBorderStrong]    = border;
    c[ImGuiCol_TableBorderLight]     = border;
    c[ImGuiCol_TableRowBg]           = {0.0f, 0.0f, 0.0f, 0.0f};
    c[ImGuiCol_TableRowBgAlt]        = {1.0f, 1.0f, 1.0f, 0.04f};
    c[ImGuiCol_TextSelectedBg]       = {ar, ag, ab, 0.35f};
    c[ImGuiCol_DragDropTarget]       = acc_hi;
    c[ImGuiCol_NavHighlight]         = acc;
    c[ImGuiCol_NavWindowingHighlight]= {1.0f, 1.0f, 1.0f, 0.70f};
    c[ImGuiCol_NavWindowingDimBg]    = {0.8f, 0.8f, 0.8f, 0.20f};
    c[ImGuiCol_ModalWindowDimBg]     = {0.0f, 0.0f, 0.0f, 0.50f};
}

} // namespace transporter::theme
