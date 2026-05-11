// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.hpp"

#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace transporter::gui {

namespace {

constexpr ImVec4 kMuted{0.65f, 0.65f, 0.70f, 1.0f};
constexpr ImVec4 kAccent{0.30f, 0.60f, 0.90f, 0.60f};

// Formats milliseconds as H:MM:SS or M:SS.
std::string format_duration(std::chrono::milliseconds ms) {
    const auto total_s = static_cast<long long>(ms.count() / 1000);
    const long long h = total_s / 3600;
    const long long m = (total_s % 3600) / 60;
    const long long s = total_s % 60;
    char buf[32];
    if (h > 0) {
        std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", h, m, s);
    } else {
        std::snprintf(buf, sizeof(buf), "%lld:%02lld", m, s);
    }
    return buf;
}

} // namespace

void draw_queue_view(AppState& st) {
    std::vector<std::filesystem::path> snap_queue;
    std::int32_t snap_index = -1;
    std::chrono::milliseconds snap_duration{0};
    std::unordered_map<std::string, std::string> display_names;
    {
        std::lock_guard<std::mutex> lk(st.queue_mtx);
        snap_queue = st.queue;
        snap_index = st.queue_index;
        snap_duration = st.queue_total_duration;
        display_names = st.queue_display_names;
    }

    // Header: count, total duration (if known), clear button
    if (snap_duration.count() > 0) {
        ImGui::Text("%zu track%s  \xc2\xb7  %s",
                    snap_queue.size(),
                    snap_queue.size() == 1 ? "" : "s",
                    format_duration(snap_duration).c_str());
    } else {
        ImGui::Text("%zu track%s", snap_queue.size(),
                    snap_queue.size() == 1 ? "" : "s");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        st.queue_clear();
    }

    ImGui::Spacing();

    if (snap_queue.empty()) {
        ImGui::TextDisabled("queue is empty");
        return;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##queue_list", ImVec2(0, avail.y), ImGuiChildFlags_Border);

    for (std::int32_t i = 0; i < static_cast<std::int32_t>(snap_queue.size()); ++i) {
        ImGui::PushID(i);

        const bool current = (i == snap_index);

        if (current) {
            ImGui::PushStyleColor(ImGuiCol_Header, kAccent);
        }

        const auto& path = snap_queue[static_cast<std::size_t>(i)];
        const auto name_it = display_names.find(path.string());
        const std::string& display = (name_it != display_names.end())
            ? name_it->second
            : path.stem().string();
        char label[512];
        std::snprintf(label, sizeof(label), "%d  %s", i + 1, display.c_str());

        if (ImGui::Selectable(label, current)) {
            st.queue_jump_to(i);
        }

        if (current) {
            ImGui::PopStyleColor();
            ImGui::SetScrollHereY(0.5f);
        }

        // Right-aligned remove button
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - 24.0f);
        if (ImGui::SmallButton("\xe2\x9c\x95")) {
            st.queue_remove(i);
        }

        ImGui::PopID();
    }

    ImGui::EndChild();
}

} // namespace transporter::gui
