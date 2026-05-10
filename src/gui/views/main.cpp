// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal main view: track block, transport row, DAC selector, bit-perfect
// indicator. Empty state is the locked first-run UX.

#include "app.hpp"

#include <transporter/engine/device.hpp>
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

void dac_combo(AppState& st) {
    ImGui::TextColored(kMuted, "DAC:");
    ImGui::SameLine();
    const char* preview = st.preferred_device.empty()
        ? "(none)" : st.preferred_device.c_str();

    if (ImGui::BeginCombo("##dac", preview)) {
        for (const auto& d : st.devices) {
            const bool selected = (d.alsa_hw_string == st.preferred_device);
            std::string label = d.fingerprint.alsa_card_name + " (" +
                                d.alsa_hw_string + ")";
            if (ImGui::Selectable(label.c_str(), selected)) {
                if (d.alsa_hw_string != st.preferred_device) {
                    st.pending_device_switch = d.alsa_hw_string;
                }
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
}

void empty_state(AppState& st) {
    ImGui::Spacing(); ImGui::Spacing();
    if (!st.devices.empty()) {
        dac_combo(st);
        ImGui::SameLine();
        if (!st.preferred_device.empty()) {
            if (ImGui::Button("Reclaim DAC")) {
                st.pending_device_switch = st.preferred_device;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Re-open exclusive ALSA hold on the selected DAC.");
            }
        }
        ImGui::Spacing();
    }
    ImGui::Indent(40.0f);
    ImGui::PushFont(nullptr);
    if (st.devices.empty()) {
        ImGui::TextDisabled("no DAC found");
        ImGui::TextColored(kMuted, "Plug in a USB DAC and restart, or set "
                                   "[device].preferred in your config:");
        ImGui::Bullet();
        ImGui::TextColored(kMuted, "~/.config/transporter/config.toml");
    } else if (st.preferred_device.empty()) {
        ImGui::TextDisabled("select a DAC above");
    }
    if (st.library_ == nullptr || st.cfg.library.roots.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("no library configured");
        ImGui::TextColored(kMuted, "Add a [library].roots entry in "
                                   "~/.config/transporter/config.toml.");
    }
    ImGui::PopFont();
    ImGui::Unindent(40.0f);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kMuted, "events:");
    const auto entries = st.snapshot_log(6);
    for (const auto& e : entries) {
        ImGui::TextWrapped("%s", e.text.c_str());
    }
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

    // Toast: shown even without an engine.
    {
        std::lock_guard lk(st.toast_mtx);
        if (st.toast.has_value()) {
            if (std::chrono::steady_clock::now() < st.toast->expires) {
                constexpr ImVec4 kToastBg{0.55f, 0.10f, 0.10f, 1.0f};
                ImGui::PushStyleColor(ImGuiCol_ChildBg, kToastBg);
                ImGui::BeginChild("##toast", ImVec2(-FLT_MIN, 28),
                                  ImGuiChildFlags_None);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
                ImGui::Text(" %s", st.toast->msg.c_str());
                ImGui::EndChild();
                ImGui::PopStyleColor();
            } else {
                st.toast.reset();
            }
        }
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
        const std::uint64_t track_frames =
            snap.output.frames_written - snap.output.frames_written_at_track_start;
        elapsed_ms = static_cast<std::int64_t>(
            (track_frames * 1000ull) / snap.source.sample_rate_hz);
    }
    char tbuf[16], dbuf[16];
    format_time(elapsed_ms, tbuf, sizeof(tbuf));
    format_time(total_ms, dbuf, sizeof(dbuf));
    ImGui::Text("%s / %s", tbuf, dbuf);
    float frac = total_ms > 0
        ? std::clamp(static_cast<float>(elapsed_ms) / static_cast<float>(total_ms), 0.0f, 1.0f)
        : 0.0f;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##seek", &frac, 0.0f, 1.0f, "") &&
        st.engine_ != nullptr && snap.source.total_frames > 0) {
        const auto target = static_cast<std::uint64_t>(
            frac * static_cast<float>(snap.source.total_frames));
        (void)st.engine_->seek(target);
    }

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
    if (ImGui::Button("Prev")) {
        if (st.engine_ != nullptr) {
            if (auto prev = st.queue_previous(); prev) {
                (void)st.engine_->load(*prev);
                (void)st.engine_->play();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Next")) {
        if (st.engine_ != nullptr) {
            if (auto next = st.queue_next(); next) {
                (void)st.engine_->load(*next);
                (void)st.engine_->play();
            }
        }
    }

    ImGui::SameLine();
    verdict_badge(snap);

    ImGui::SameLine();
    if (ImGui::Button("Release DAC")) {
        st.release_dac_requested = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Stop playback and release exclusive ALSA hold so "
                          "other apps can use the DAC.");
    }

    ImGui::Spacing();

    dac_combo(st);

    // HW volume slider — cached at ~4 Hz to avoid opening ALSA mixer every frame
    ImGui::Spacing();
    {
        const auto& hw_string = snap.device.current_hw_string;
        const auto& vol = snap.device.capabilities.hw_volume;
        if (vol.present && !hw_string.empty()) {
            const auto now = std::chrono::steady_clock::now();
            if (st.hw_volume_pct < 0 ||
                now - st.hw_volume_last_poll > std::chrono::milliseconds(250)) {
                st.hw_volume_pct = engine::get_hw_volume_pct(hw_string);
                if (st.hw_volume_pct < 0) { st.hw_volume_pct = 0; }
                // Also poll mute state via same mechanism (pct returns -1 if control absent)
                st.hw_volume_last_poll = now;
            }
            int pct = st.hw_volume_pct;
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderInt("Vol", &pct, 0, 100, "%d%%")) {
                st.hw_volume_pct = pct;
                engine::set_hw_volume_pct(hw_string, pct);
            }
            ImGui::SameLine();
            if (ImGui::Button("Mute")) {
                engine::toggle_hw_mute(hw_string);
                st.hw_volume_last_poll = {};  // force re-poll next frame
            }
            if (vol.has_db_scale) {
                ImGui::SameLine();
                ImGui::TextColored(kMuted, "HW  %.0f..%.0f dB",
                    static_cast<float>(vol.min_db_x100) / 100.0f,
                    static_cast<float>(vol.max_db_x100) / 100.0f);
            }
        } else if (!hw_string.empty()) {
            ImGui::TextColored(kMuted, "Vol: no HW control — use alsamixer");
        }
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
