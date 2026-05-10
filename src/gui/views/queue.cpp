// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.hpp"

#include <imgui.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace transporter::gui {

namespace {

constexpr ImVec4 kMuted{0.65f, 0.65f, 0.70f, 1.0f};
constexpr ImVec4 kAccent{0.30f, 0.60f, 0.90f, 0.60f};

} // namespace

void draw_queue_view(AppState& st) {
    std::vector<std::filesystem::path> snap_queue;
    std::int32_t snap_index = -1;
    {
        std::lock_guard<std::mutex> lk(st.queue_mtx);
        snap_queue = st.queue;
        snap_index = st.queue_index;
    }

    // Header: count + clear button on same line
    ImGui::Text("%zu track%s", snap_queue.size(),
                snap_queue.size() == 1 ? "" : "s");
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

        char label[512];
        std::snprintf(label, sizeof(label), "%d  %s",
                      i + 1,
                      snap_queue[i].stem().string().c_str());

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
