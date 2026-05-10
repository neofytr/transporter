// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal main view: track block, transport row, DAC selector, bit-perfect
// indicator. Empty state is the locked first-run UX.

#include "app.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/engine/telemetry.hpp>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace transporter::gui {

namespace {

constexpr ImVec4 kPass{0.40f, 0.85f, 0.40f, 1.0f};
constexpr ImVec4 kFail{0.95f, 0.40f, 0.35f, 1.0f};
constexpr ImVec4 kWarn{0.95f, 0.75f, 0.30f, 1.0f};
constexpr ImVec4 kMuted{0.65f, 0.65f, 0.70f, 1.0f};

void format_time(std::int64_t ms, char* out, std::size_t n) {
    if (ms < 0) ms = 0;
    const std::int64_t s = ms / 1000;
    std::snprintf(out, n, "%lld:%02lld",
                  static_cast<long long>(s / 60),
                  static_cast<long long>(s % 60));
}

void empty_state(AppState& st) {
    ImGui::Spacing(); ImGui::Spacing();
    ImGui::Indent(40.0f);
    ImGui::PushFont(nullptr);
    ImGui::TextDisabled("no DAC selected");
    if (st.devices.empty()) {
        ImGui::TextColored(kMuted, "Plug in a USB DAC and restart, or set "
                                   "[device].preferred in your config:");
        ImGui::Bullet();
        ImGui::TextColored(kMuted, "~/.config/transporter/config.toml");
    }
    if (st.library_ == nullptr || st.cfg.library.roots.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("no library configured");
        ImGui::TextColored(kMuted, "Add a [library].roots entry in "
                                   "~/.config/transporter/config.toml.");
    }
    ImGui::PopFont();
    ImGui::Unindent(40.0f);
}

void verdict_badge(const engine::PipelineSnapshot& s) {
    using L = engine::BitPerfectVerdict::Level;
    ImVec4 col = kFail;
    const char* tag = "NO";
    if (s.bit_perfect.level == L::Yes) {
        col = kPass;
        tag = "YES";
    } else if (s.bit_perfect.level == L::Qualified) {
        col = kWarn;
        tag = "QUAL";
    }
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
    ImGui::SmallButton(tag);
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("digital path bit-perfect: %s",
                    s.bit_perfect.digital_path_bitperfect ? "yes" : "no");
        ImGui::Text("no resampling in flight: %s",
                    s.bit_perfect.no_resampling_in_flight ? "yes" : "no");
        ImGui::Text("RT enabled: %s",
                    s.bit_perfect.rt_enabled ? "yes" : "no");
        ImGui::Text("no recent xrun: %s",
                    s.bit_perfect.no_recent_xrun ? "yes" : "no");
        ImGui::Text("volume at unity: %s",
                    s.bit_perfect.volume_unity ? "yes" : "no");
        ImGui::Text("no mismatch in flight: %s",
                    s.bit_perfect.no_mismatch_in_flight ? "yes" : "no");
        for (const auto& q : s.bit_perfect.qualifications) {
            ImGui::BulletText("%s", q.c_str());
        }
        ImGui::EndTooltip();
    }
}

} // namespace

void draw_main_view(AppState& st) {
    engine::PipelineSnapshot snap{};
    if (st.engine_) {
        snap = st.engine_->pipeline_snapshot();
    }

    if (st.engine_ == nullptr) {
        empty_state(st);
        return;
    }

    // Track block
    ImGui::PushFont(nullptr);
    if (!snap.source.tags.title.empty()) {
        ImGui::Text("%s", snap.source.tags.title.c_str());
    } else if (!snap.source.file_path.empty()) {
        ImGui::Text("%s", snap.source.file_path.c_str());
    } else {
        ImGui::TextDisabled("(no track loaded)");
    }
    ImGui::PopFont();

    if (!snap.source.tags.artist.empty()) {
        ImGui::TextColored(kMuted, "%s", snap.source.tags.artist.c_str());
    }
    if (!snap.source.tags.album.empty()) {
        ImGui::TextColored(kMuted, "%s", snap.source.tags.album.c_str());
    }

    // Time / progress
    const std::int64_t total_ms = snap.source.duration.count();
    std::int64_t elapsed_ms = 0;
    if (snap.source.sample_rate_hz > 0) {
        elapsed_ms = static_cast<std::int64_t>(
            (snap.output.frames_written * 1000ull) / snap.source.sample_rate_hz);
    }
    char tbuf[16], dbuf[16];
    format_time(elapsed_ms, tbuf, sizeof(tbuf));
    format_time(total_ms, dbuf, sizeof(dbuf));
    ImGui::Text("%s / %s", tbuf, dbuf);
    const float frac = total_ms > 0
        ? std::clamp(static_cast<float>(elapsed_ms) / static_cast<float>(total_ms), 0.0f, 1.0f)
        : 0.0f;
    ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 6.0f), "");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Transport row
    const bool playing = (snap.engine_state == engine::State::Playing);
    if (ImGui::Button(playing ? "Pause" : "Play")) {
        if (playing) {
            (void)st.engine_->pause();
        } else {
            (void)st.engine_->play();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        (void)st.engine_->stop();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(true);
    ImGui::Button("Prev");
    ImGui::SameLine();
    ImGui::Button("Next");
    ImGui::EndDisabled();

    ImGui::SameLine();
    verdict_badge(snap);

    ImGui::Spacing();

    // DAC selector
    ImGui::TextColored(kMuted, "DAC:");
    ImGui::SameLine();
    const char* preview = st.preferred_device.empty()
        ? (snap.device.current_hw_string.empty() ? "(none)"
                                                  : snap.device.current_hw_string.c_str())
        : st.preferred_device.c_str();

    if (ImGui::BeginCombo("##dac", preview)) {
        for (const auto& d : st.devices) {
            const bool selected = (d.alsa_hw_string == preview);
            std::string label = d.fingerprint.alsa_card_name + " (" +
                                d.alsa_hw_string + ")";
            if (ImGui::Selectable(label.c_str(), selected)) {
                if (d.alsa_hw_string != st.preferred_device) {
                    st.preferred_device = d.alsa_hw_string;
                    st.push_log("DAC change requested: " + d.alsa_hw_string +
                                " (restart recommended)");
                }
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    if (st.preferred_device != snap.device.current_hw_string &&
        !st.preferred_device.empty() &&
        !snap.device.current_hw_string.empty()) {
        ImGui::TextColored(kWarn,
                           "DAC change pending; restart transporter to apply.");
    }

    // Event log preview
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kMuted, "events:");
    const auto entries = st.snapshot_log(6);
    for (const auto& e : entries) {
        ImGui::TextWrapped("%s", e.text.c_str());
    }
}

} // namespace transporter::gui
