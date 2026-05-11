// SPDX-License-Identifier: GPL-3.0-or-later

#include "theme.hpp"

#include <imgui.h>

namespace transporter::gui {

void apply_theme(const DominantColor& dc) {
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowPadding     = ImVec2(28, 24);
    s.FramePadding      = ImVec2(12, 8);
    s.ItemSpacing       = ImVec2(12, 12);
    s.ItemInnerSpacing  = ImVec2(8, 8);
    s.CellPadding       = ImVec2(8, 6);
    s.IndentSpacing     = 22.0f;
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 10.0f;

    s.WindowRounding    = 12.0f;
    s.ChildRounding     = 10.0f;
    s.FrameRounding     = 8.0f;
    s.GrabRounding      = 8.0f;
    s.PopupRounding     = 8.0f;
    s.ScrollbarRounding = 8.0f;
    s.TabRounding       = 8.0f;
    s.WindowBorderSize  = 0.0f;
    s.ChildBorderSize   = 0.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 0.0f;
    s.TabBorderSize     = 0.0f;
    s.SeparatorTextBorderSize = 1.0f;

    // Slightly lighter elevation for child panels (toast, banner) over bg.
    const ImVec4 child_bg = ImVec4(
        dc.muted_bg.x + 0.030f,
        dc.muted_bg.y + 0.030f,
        dc.muted_bg.z + 0.030f, 1.0f);
    const ImVec4 popup_bg = ImVec4(
        dc.muted_bg.x + 0.050f,
        dc.muted_bg.y + 0.050f,
        dc.muted_bg.z + 0.050f, 1.0f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = dc.text;
    c[ImGuiCol_TextDisabled]          = dc.text_dim;
    c[ImGuiCol_WindowBg]              = dc.muted_bg;
    c[ImGuiCol_ChildBg]               = child_bg;
    c[ImGuiCol_PopupBg]               = popup_bg;
    c[ImGuiCol_Border]                = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg]               = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(1, 1, 1, 0.14f);

    c[ImGuiCol_TitleBg]               = dc.muted_bg;
    c[ImGuiCol_TitleBgActive]         = dc.muted_bg;
    c[ImGuiCol_TitleBgCollapsed]      = dc.muted_bg;
    c[ImGuiCol_MenuBarBg]             = child_bg;

    c[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(1, 1, 1, 0.16f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(1, 1, 1, 0.24f);

    c[ImGuiCol_CheckMark]             = dc.accent;
    c[ImGuiCol_SliderGrab]            = dc.accent;
    c[ImGuiCol_SliderGrabActive]      = dc.accent;

    // Transport / pill buttons are drawn manually; ImGui buttons here are the
    // fallback for context menus and tab pills. Keep them transparent so
    // glyph-over-tinted-bg reads cleanly.
    c[ImGuiCol_Button]                = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ButtonHovered]         = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_ButtonActive]          = ImVec4(1, 1, 1, 0.14f);

    c[ImGuiCol_Header]                = ImVec4(1, 1, 1, 0.05f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_HeaderActive]          = ImVec4(1, 1, 1, 0.16f);

    c[ImGuiCol_Separator]             = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_SeparatorActive]       = dc.accent;

    c[ImGuiCol_ResizeGrip]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_ResizeGripActive]      = dc.accent;

    c[ImGuiCol_Tab]                   = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered]            = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_TabActive]             = dc.accent;
    c[ImGuiCol_TabUnfocused]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabUnfocusedActive]    = dc.accent;

    c[ImGuiCol_PlotLines]             = dc.accent;
    c[ImGuiCol_PlotLinesHovered]      = dc.accent;
    c[ImGuiCol_PlotHistogram]         = dc.accent;
    c[ImGuiCol_PlotHistogramHovered]  = dc.accent;

    c[ImGuiCol_TableHeaderBg]         = child_bg;
    c[ImGuiCol_TableBorderStrong]     = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_TableBorderLight]      = ImVec4(1, 1, 1, 0.04f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1, 1, 1, 0.03f);

    c[ImGuiCol_TextSelectedBg]        = ImVec4(dc.accent.x, dc.accent.y, dc.accent.z, 0.35f);
    c[ImGuiCol_DragDropTarget]        = dc.accent;
    c[ImGuiCol_NavHighlight]          = dc.accent;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.50f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0, 0, 0, 0.40f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.55f);
}

} // namespace transporter::gui
