// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal main view: track block, transport row, DAC selector, bit-perfect
// indicator. Empty state is the locked first-run UX.

#include "app.hpp"
#include "lyrics.hpp"
#include "spectrum.hpp"

#include <transporter/engine/device.hpp>
#include <transporter/engine/engine.hpp>
#include <transporter/engine/ring.hpp>
#include <transporter/engine/telemetry.hpp>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <string>

namespace transporter::gui {

static void format_time(std::int64_t ms, char* out, std::size_t n) {
    if (ms < 0) ms = 0;
    const std::int64_t s  = ms / 1000;
    const std::int64_t m  = s / 60;
    const std::int64_t h  = m / 60;
    if (h > 0) {
        std::snprintf(out, n, "%lld:%02lld:%02lld",
                      static_cast<long long>(h),
                      static_cast<long long>(m % 60),
                      static_cast<long long>(s % 60));
    } else {
        std::snprintf(out, n, "%lld:%02lld",
                      static_cast<long long>(m),
                      static_cast<long long>(s % 60));
    }
}

namespace {

constexpr ImVec4 kPass{0.40f, 0.85f, 0.40f, 1.0f};
constexpr ImVec4 kFail{0.95f, 0.40f, 0.35f, 1.0f};
constexpr ImVec4 kWarn{0.95f, 0.75f, 0.30f, 1.0f};
constexpr ImVec4 kMuted{0.65f, 0.65f, 0.70f, 1.0f};

struct SpectrumState {
    static constexpr std::size_t FFT_N = 1024;
    static constexpr int BARS = 32;

    std::array<float, FFT_N> sample_buf{};
    std::size_t sample_pos = 0;
    std::array<float, BARS> magnitudes{};
    std::array<float, BARS> peaks{};

    void ingest(const float* samples, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            sample_buf[sample_pos % FFT_N] = samples[i];
            ++sample_pos;
        }
    }

    void compute(std::uint32_t sample_rate_hz) {
        if (sample_pos < FFT_N) return;

        std::array<float, FFT_N> windowed;
        for (std::size_t i = 0; i < FFT_N; ++i)
            windowed[i] = sample_buf[(sample_pos - FFT_N + i) % FFT_N];
        apply_hann(windowed);

        std::array<std::complex<float>, FFT_N> cx{};
        for (std::size_t i = 0; i < FFT_N; ++i)
            cx[i] = {windowed[i], 0.0f};
        fft(cx);

        const float rate = static_cast<float>(sample_rate_hz > 0 ? sample_rate_hz : 44100);
        const float bin_hz = rate / static_cast<float>(FFT_N);
        const float log_lo = std::log10(20.0f);
        const float log_hi = std::log10(20000.0f);

        for (int b = 0; b < BARS; ++b) {
            const float lo_hz = std::pow(10.0f, log_lo + (log_hi - log_lo) * static_cast<float>(b)     / BARS);
            const float hi_hz = std::pow(10.0f, log_lo + (log_hi - log_lo) * static_cast<float>(b + 1) / BARS);
            const int bin_lo = std::max(1, static_cast<int>(lo_hz / bin_hz));
            const int bin_hi = std::min(static_cast<int>(FFT_N / 2),
                                        static_cast<int>(hi_hz / bin_hz) + 1);

            float mag = 0.0f;
            for (int k = bin_lo; k < bin_hi; ++k)
                mag = std::max(mag, std::abs(cx[k]));
            float db = mag > 0.0f ? 20.0f * std::log10(mag / static_cast<float>(FFT_N / 2)) : -90.0f;
            float norm = std::clamp((db + 90.0f) / 90.0f, 0.0f, 1.0f);
            magnitudes[b] = norm > magnitudes[b] ? norm : magnitudes[b] * 0.85f;
            if (norm >= peaks[b]) peaks[b] = norm;
            else peaks[b] = std::max(0.0f, peaks[b] - 0.01f);
        }
    }
};

static SpectrumState g_spectrum;

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
    if (s.bit_perfect.level == L::Yes) {
        col = kPass;
    } else if (s.bit_perfect.level == L::Qualified) {
        col = kWarn;
    }
    // Draw a small filled circle using ImDrawList; Dummy creates the hover region.
    const float radius = 6.0f;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float cy = cursor.y + ImGui::GetTextLineHeight() * 0.5f;
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(cursor.x + radius, cy), radius,
        ImGui::ColorConvertFloat4ToU32(col));
    ImGui::Dummy(ImVec2(radius * 2.0f + 4.0f, ImGui::GetTextLineHeight()));
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

    // Mini mode toggle — top right of main view
    {
        const float btn_w = ImGui::CalcTextSize("mini").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btn_w);
        if (ImGui::SmallButton(st.mini_mode ? "full" : "mini")) {
            st.mini_mode = !st.mini_mode;
        }
        ImGui::SameLine(0, 0);
        ImGui::NewLine();
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

    // Queue position
    {
        std::lock_guard lk(st.queue_mtx);
        if (st.queue.size() > 1 && st.queue_index >= 0) {
            ImGui::SameLine();
            ImGui::TextColored(kMuted, "[%d / %d]",
                               st.queue_index + 1,
                               static_cast<int>(st.queue.size()));
        }
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
    char tbuf[24], dbuf[24];
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
    // Waveform overlay — drawn into the seek slider's bounding rect.
    if (st.waveform_ready.load(std::memory_order_acquire)) {
        const ImVec2 rmin = ImGui::GetItemRectMin();
        const ImVec2 rmax = ImGui::GetItemRectMax();
        std::lock_guard lk(st.waveform_mtx);
        if (!st.waveform.empty()) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float w   = rmax.x - rmin.x;
            const float h   = rmax.y - rmin.y;
            const float cy  = (rmin.y + rmax.y) * 0.5f;
            const auto  N   = static_cast<float>(st.waveform.size());
            const float bw  = w / N;
            constexpr ImU32 kWave = IM_COL32(100, 200, 140, 55);
            for (std::size_t i = 0; i < st.waveform.size(); ++i) {
                const float v  = st.waveform[i];
                const float x0 = rmin.x + static_cast<float>(i) * bw;
                const float hh = v * h * 0.5f;
                dl->AddRectFilled({x0, cy - hh}, {x0 + bw, cy + hh}, kWave);
            }
        }
    }

    if (snap.ring.capacity_bytes > 0) {
        const float ring_fill = std::clamp(
            static_cast<float>(snap.ring.fill_bytes) /
            static_cast<float>(snap.ring.capacity_bytes),
            0.0f, 1.0f);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::ProgressBar(ring_fill, ImVec2(-FLT_MIN, 3.0f), "");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("ring buffer %.0f%% full  (%zu / %zu bytes)",
                              ring_fill * 100.0f,
                              snap.ring.fill_bytes,
                              snap.ring.capacity_bytes);
        }
    }

    // Spectrum analyser
    if (st.engine_ != nullptr) {
        const auto* sring = st.engine_->spectrum_ring();
        if (sring != nullptr) {
            auto* rw_ring = const_cast<engine::SpscByteRing*>(sring);
            float tmp[512];
            const std::size_t avail = rw_ring->readable();
            if (avail >= sizeof(float)) {
                const std::size_t take = std::min(avail, sizeof(tmp));
                auto sp = std::span<std::byte>(reinterpret_cast<std::byte*>(tmp), take);
                const std::size_t got = rw_ring->read(sp);
                g_spectrum.ingest(tmp, got / sizeof(float));
            }
            g_spectrum.compute(snap.source.sample_rate_hz);
        }
    }

    // Draw bars
    {
        const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
        const float bar_total_w = ImGui::GetContentRegionAvail().x;
        const float bar_h_max = 48.0f;
        const float bar_w = bar_total_w / static_cast<float>(SpectrumState::BARS);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        constexpr ImU32 kBarCol  = IM_COL32(80, 200, 120, 200);
        constexpr ImU32 kPeakCol = IM_COL32(200, 240, 200, 180);

        for (int b = 0; b < SpectrumState::BARS; ++b) {
            const float x0 = canvas_pos.x + b * bar_w + 1.0f;
            const float x1 = canvas_pos.x + (b + 1) * bar_w - 1.0f;
            const float mag = g_spectrum.magnitudes[b];
            const float pk  = g_spectrum.peaks[b];
            const float bot = canvas_pos.y + bar_h_max;
            const float top = canvas_pos.y + bar_h_max * (1.0f - mag);
            if (top < bot)
                dl->AddRectFilled({x0, top}, {x1, bot}, kBarCol);
            const float pk_y = canvas_pos.y + bar_h_max * (1.0f - pk);
            if (pk > 0.01f)
                dl->AddLine({x0, pk_y}, {x1, pk_y}, kPeakCol);
        }
        ImGui::Dummy({bar_total_w, bar_h_max + 2.0f});
    }

    // Lyrics — load on track change, display current line.
    {
        const std::filesystem::path cur_path{snap.source.file_path};
        if (cur_path != st.lyrics_loaded_for) {
            st.lyrics_loaded_for = cur_path;
            st.lyrics = cur_path.empty() ? Lyrics{} : load_lyrics(cur_path);
        }
        if (st.lyrics.loaded) {
            const int li = current_lyric_line(st.lyrics, elapsed_ms);
            ImGui::Spacing();
            if (ImGui::CollapsingHeader("Lyrics", ImGuiTreeNodeFlags_DefaultOpen)) {
                const ImVec2 avail = ImGui::GetContentRegionAvail();
                ImGui::BeginChild("##lyrics", ImVec2(0, std::min(avail.y, 120.0f)),
                                  ImGuiChildFlags_Border);
                constexpr ImVec4 kActive{0.95f, 0.95f, 0.95f, 1.0f};
                constexpr ImVec4 kDim{0.55f, 0.55f, 0.60f, 1.0f};
                for (int i = 0; i < static_cast<int>(st.lyrics.lines.size()); ++i) {
                    const bool active = (i == li);
                    ImGui::TextColored(active ? kActive : kDim,
                                       "%s", st.lyrics.lines[static_cast<std::size_t>(i)].text.c_str());
                    if (active) ImGui::SetScrollHereY(0.5f);
                }
                ImGui::EndChild();
            }
        }
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

    ImGui::SameLine();
    // Shuffle toggle
    if (st.shuffle) ImGui::PushStyleColor(ImGuiCol_Button, kPass);
    if (ImGui::SmallButton("shuf")) {
        st.queue_shuffle_toggle();
    }
    if (st.shuffle) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("shuffle: %s", st.shuffle ? "on" : "off");

    ImGui::SameLine();
    // Repeat mode cycling: None → One → All → None
    const char* rep_label = "rep:off";
    if (st.repeat_mode == AppState::RepeatMode::One) rep_label = "rep:1";
    else if (st.repeat_mode == AppState::RepeatMode::All) rep_label = "rep:all";
    const bool rep_active = (st.repeat_mode != AppState::RepeatMode::None);
    if (rep_active) ImGui::PushStyleColor(ImGuiCol_Button, kPass);
    if (ImGui::SmallButton(rep_label)) {
        switch (st.repeat_mode) {
        case AppState::RepeatMode::None: st.repeat_mode = AppState::RepeatMode::One; break;
        case AppState::RepeatMode::One:  st.repeat_mode = AppState::RepeatMode::All; break;
        case AppState::RepeatMode::All:  st.repeat_mode = AppState::RepeatMode::None; break;
        }
    }
    if (rep_active) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("repeat mode");

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

void draw_mini_view(AppState& st) {
    engine::PipelineSnapshot snap{};
    if (st.engine_) snap = st.engine_->pipeline_snapshot();

    if (ImGui::SmallButton("full")) { st.mini_mode = false; }
    ImGui::SameLine();

    const char* title = snap.source.tags.title.empty()
        ? (snap.source.file_path.empty() ? "\xe2\x80\x94" : snap.source.file_path.c_str())
        : snap.source.tags.title.c_str();
    ImGui::TextColored(kMuted, "%s", title);
    ImGui::SameLine();

    const std::int64_t total_ms = snap.source.duration.count();
    const std::int64_t fw = static_cast<std::int64_t>(snap.output.frames_written) -
                            static_cast<std::int64_t>(snap.output.frames_written_at_track_start);
    const std::int64_t elapsed_ms = snap.source.sample_rate_hz > 0
        ? (fw * 1000LL) / snap.source.sample_rate_hz : 0;
    char tbuf[24], dbuf[24];
    format_time(elapsed_ms < 0 ? 0 : elapsed_ms, tbuf, sizeof(tbuf));
    format_time(total_ms, dbuf, sizeof(dbuf));
    ImGui::TextColored(kMuted, "%s/%s", tbuf, dbuf);
    ImGui::SameLine();

    const bool playing = (snap.engine_state == engine::State::Playing);
    if (st.engine_) {
        if (ImGui::SmallButton(playing ? "\xe2\x8f\xb8" : "\xe2\x8f\xb5")) {
            playing ? (void)st.engine_->pause() : (void)st.engine_->play();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("\xe2\x8f\xb9")) { (void)st.engine_->stop(); }
        ImGui::SameLine();
        if (ImGui::SmallButton("\xe2\x8f\xae")) {
            if (auto p = st.queue_previous(); p) { (void)st.engine_->load(*p); (void)st.engine_->play(); }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("\xe2\x8f\xad")) {
            if (auto n = st.queue_next(); n) { (void)st.engine_->load(*n); (void)st.engine_->play(); }
        }
    }
}

} // namespace transporter::gui
