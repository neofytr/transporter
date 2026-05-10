// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pipeline transparency view. Per locked.md → "Pipeline transparency view":
// dense by design, eight cards in fixed top-down order, every PipelineSnapshot
// field surfaced without abbreviation. This is the single UI surface exempt
// from the minimalism rule.

#include "app.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/engine/telemetry.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace transporter::gui {

namespace {

using engine::PipelineSnapshot;
using engine::SampleFormat;
using engine::sample_format_name;

constexpr ImVec4 kHeaderBg{0.15f, 0.18f, 0.22f, 1.0f};
constexpr ImVec4 kPass{0.40f, 0.85f, 0.40f, 1.0f};
constexpr ImVec4 kFail{0.95f, 0.40f, 0.35f, 1.0f};
constexpr ImVec4 kWarn{0.95f, 0.75f, 0.30f, 1.0f};
constexpr ImVec4 kMuted{0.65f, 0.65f, 0.70f, 1.0f};

const char* state_name(engine::State s) {
    switch (s) {
    case engine::State::Idle:         return "Idle";
    case engine::State::Loading:      return "Loading";
    case engine::State::Playing:      return "Playing";
    case engine::State::Paused:       return "Paused";
    case engine::State::Stopped:      return "Stopped";
    case engine::State::Error:        return "Error";
    case engine::State::Disconnected: return "Disconnected";
    }
    return "?";
}

void card_begin(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.11f, 0.13f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::BeginChild(label, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY |
                                              ImGuiChildFlags_Border);
    ImGui::PushStyleColor(ImGuiCol_Header, kHeaderBg);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kHeaderBg);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, kHeaderBg);
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
    ImGui::PopStyleColor(3);
    ImGui::Separator();
}

void card_end() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

void kv(const char* k, std::string_view v) {
    ImGui::TextColored(kMuted, "%s", k);
    ImGui::SameLine(180.0f);
    if (v.empty()) {
        ImGui::TextDisabled("(none)");
    } else {
        ImGui::TextUnformatted(v.data(), v.data() + v.size());
    }
}

void kv_u64(const char* k, std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    kv(k, buf);
}

void kv_size(const char* k, std::size_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%zu", v);
    kv(k, buf);
}

void kv_u32(const char* k, std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", v);
    kv(k, buf);
}

void kv_int(const char* k, int v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", v);
    kv(k, buf);
}

void kv_bool(const char* k, bool v) {
    ImGui::TextColored(kMuted, "%s", k);
    ImGui::SameLine(180.0f);
    ImGui::TextColored(v ? kPass : kFail, "%s", v ? "yes" : "no");
}

std::string truncate_middle(std::string_view s, std::size_t budget) {
    if (s.size() <= budget) {
        return std::string{s};
    }
    if (budget < 5) {
        return std::string{s.substr(0, budget)};
    }
    const std::size_t keep = budget - 3;
    const std::size_t left = keep / 2;
    const std::size_t right = keep - left;
    std::string out;
    out.append(s.substr(0, left));
    out.append("...");
    out.append(s.substr(s.size() - right));
    return out;
}

void format_axis(const char* label, std::string_view declared,
                 std::string_view matched, bool ok) {
    ImGui::TextColored(kMuted, "%s", label);
    ImGui::SameLine(120.0f);
    ImGui::TextColored(ok ? kPass : kFail, "%s", ok ? "[+]" : "[!]");
    ImGui::SameLine();
    ImGui::Text("%.*s -> %.*s",
                static_cast<int>(declared.size()), declared.data(),
                static_cast<int>(matched.size()), matched.data());
}

void source_card(const PipelineSnapshot& s) {
    card_begin("Source");
    kv("file path", truncate_middle(s.source.file_path, 96));
    kv("codec", s.source.codec_name);
    kv("container", s.source.container);
    kv("decoder lib", s.source.decoder_lib_version);
    kv_u32("bitrate kbps", s.source.bitrate_kbps);
    kv_u32("file bit depth", s.source.bit_depth_file);
    kv_u32("channels", s.source.channels);
    kv_u32("native rate Hz", s.source.sample_rate_hz);
    kv_u64("total frames", s.source.total_frames);
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld ms",
                      static_cast<long long>(s.source.duration.count()));
        kv("duration", buf);
    }
    kv("tag artist", s.source.tags.artist);
    kv("tag album", s.source.tags.album);
    kv("tag title", s.source.tags.title);
    kv("tag track", s.source.tags.track_no);
    kv("tag date", s.source.tags.date);
    card_end();
}

void decoder_card(const PipelineSnapshot& s) {
    card_begin("Decoder");
    char fmt[64];
    std::snprintf(fmt, sizeof(fmt), "%u Hz / %s / %u ch",
                  s.decoder.output_format.sample_rate_hz,
                  std::string(sample_format_name(s.decoder.output_format.sample_format)).c_str(),
                  s.decoder.output_format.channels);
    kv("output format", fmt);
    kv_u64("frames produced", s.decoder.frames_produced);
    kv("thread state", s.decoder.thread_state);
    card_end();
}

void format_match_card(const PipelineSnapshot& s) {
    card_begin("Format match");

    if (s.format_match.matched_ok) {
        ImGui::TextColored(kPass, "matched");
    } else {
        ImGui::TextColored(kFail, "REFUSED");
        ImGui::SameLine();
        ImGui::TextColored(kFail, "%s", s.format_match.rejection_reason.c_str());
    }
    ImGui::Spacing();

    char dr[24], mr[24];
    std::snprintf(dr, sizeof(dr), "%u", s.format_match.declared.sample_rate_hz);
    std::snprintf(mr, sizeof(mr), "%u", s.format_match.matched.sample_rate_hz);
    format_axis("rate", dr, mr,
                s.format_match.declared.sample_rate_hz == s.format_match.matched.sample_rate_hz);

    format_axis("sample fmt",
                sample_format_name(s.format_match.declared.sample_format),
                sample_format_name(s.format_match.matched.sample_format),
                s.format_match.declared.sample_format == s.format_match.matched.sample_format);

    char dc[24], mc[24];
    std::snprintf(dc, sizeof(dc), "%u", s.format_match.declared.channels);
    std::snprintf(mc, sizeof(mc), "%u", s.format_match.matched.channels);
    format_axis("channels", dc, mc,
                s.format_match.declared.channels == s.format_match.matched.channels);

    ImGui::Spacing();
    ImGui::TextColored(kMuted, "device formats:");
    ImGui::SameLine();
    bool first = true;
    for (auto f : s.format_match.device_format_set) {
        if (!first) ImGui::SameLine();
        ImGui::TextUnformatted(std::string(sample_format_name(f)).c_str());
        first = false;
    }
    ImGui::TextColored(kMuted, "device rates:");
    ImGui::SameLine();
    first = true;
    for (auto r : s.format_match.device_rate_set) {
        if (!first) ImGui::SameLine();
        ImGui::Text("%u", r);
        first = false;
    }
    card_end();
}

void ring_card(const PipelineSnapshot& s) {
    card_begin("Ring");
    kv_size("capacity bytes", s.ring.capacity_bytes);
    kv_size("fill bytes", s.ring.fill_bytes);
    kv_size("fill frames", s.ring.fill_frames);
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld us",
                      static_cast<long long>(s.ring.fill_us.count()));
        kv("fill time", buf);
    }
    kv_size("max watermark", s.ring.max_watermark_bytes);
    if (s.ring.capacity_bytes > 0) {
        const float frac = static_cast<float>(s.ring.fill_bytes) /
                           static_cast<float>(s.ring.capacity_bytes);
        ImGui::ProgressBar(std::clamp(frac, 0.0f, 1.0f),
                           ImVec2(-FLT_MIN, 0.0f), "");
    }
    card_end();
}

void output_card(const PipelineSnapshot& s) {
    card_begin("Output");
    kv_u32("period frames", s.output.period_size_frames);
    kv_u32("periods", s.output.periods);
    kv_u32("buffer frames", s.output.buffer_size_frames);
    char fmt[64];
    std::snprintf(fmt, sizeof(fmt), "%u Hz / %s / %u ch",
                  s.output.hw_params_set.sample_rate_hz,
                  std::string(sample_format_name(s.output.hw_params_set.sample_format)).c_str(),
                  s.output.hw_params_set.channels);
    kv("hw_params set", fmt);
    kv_u64("frames written", s.output.frames_written);
    ImGui::TextColored(kMuted, "%s", "xrun count");
    ImGui::SameLine(180.0f);
    ImGui::TextColored(s.output.xrun_count == 0 ? kPass : kFail,
                       "%u", s.output.xrun_count);
    card_end();
}

void device_card(const PipelineSnapshot& s) {
    card_begin("Device");
    kv("hw string", s.device.current_hw_string);
    kv("alsa name", s.device.fingerprint.alsa_card_name);
    kv("alsa longname", s.device.fingerprint.alsa_card_longname);
    if (s.device.fingerprint.is_usb) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s:%s",
                      s.device.fingerprint.usb_vendor_id.c_str(),
                      s.device.fingerprint.usb_product_id.c_str());
        kv("USB vendor:product", buf);
        kv("USB serial", s.device.fingerprint.usb_serial);
    } else {
        kv("USB", "not USB");
    }
    kv_bool("connected", s.device.is_connected);
    if (s.device.last_disconnected_at.time_since_epoch().count() != 0) {
        const auto ms_ago = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - s.device.last_disconnected_at).count();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld ms ago",
                      static_cast<long long>(ms_ago));
        kv("last disconnect", buf);
    }

    ImGui::Spacing();
    ImGui::TextColored(kMuted, "supported formats:");
    ImGui::SameLine();
    bool first = true;
    for (auto f : s.device.capabilities.formats) {
        if (!first) ImGui::SameLine();
        ImGui::TextUnformatted(std::string(sample_format_name(f)).c_str());
        first = false;
    }

    ImGui::TextColored(kMuted, "supported rates:");
    ImGui::SameLine();
    first = true;
    for (auto r : s.device.capabilities.sample_rates) {
        if (!first) ImGui::SameLine();
        ImGui::Text("%u", r);
        first = false;
    }
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%u .. %u",
                      s.device.capabilities.channels_min,
                      s.device.capabilities.channels_max);
        kv("channels", buf);
    }
    if (s.device.capabilities.hw_volume.present) {
        char buf[96];
        if (s.device.capabilities.hw_volume.has_db_scale) {
            std::snprintf(buf, sizeof(buf), "[%s]  %.2f dB .. %.2f dB",
                          s.device.capabilities.hw_volume.element_name.c_str(),
                          static_cast<double>(s.device.capabilities.hw_volume.min_db_x100) / 100.0,
                          static_cast<double>(s.device.capabilities.hw_volume.max_db_x100) / 100.0);
        } else {
            std::snprintf(buf, sizeof(buf), "[%s]  uncalibrated",
                          s.device.capabilities.hw_volume.element_name.c_str());
        }
        kv("HW volume", buf);
    } else {
        kv("HW volume", "(none)");
    }
    card_end();
}

void realtime_card(const PipelineSnapshot& s) {
    card_begin("Realtime");
    kv("policy", s.realtime.status.mode == engine::rt::Mode::Fifo
                     ? "SCHED_FIFO" : "SCHED_OTHER");
    kv_int("priority", s.realtime.status.priority);
    kv_bool("mlocked", s.realtime.status.memlocked);
    kv_int("cpu pinned", s.realtime.status.cpu_pinned);
    kv("fallback reason", s.realtime.status.fallback_reason);
    kv_u64("trace dropped", s.realtime.trace_dropped);
    card_end();
}

void verdict_card(const PipelineSnapshot& s) {
    card_begin("Bit-perfect verdict");
    using L = engine::BitPerfectVerdict::Level;

    ImVec4 col = kFail;
    const char* tag = "NO";
    if (s.bit_perfect.level == L::Yes) {
        col = kPass;
        tag = "YES";
    } else if (s.bit_perfect.level == L::Qualified) {
        col = kWarn;
        tag = "QUALIFIED";
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(20, 12));
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
    ImGui::Button(tag, ImVec2(160, 0));
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    ImGui::Spacing();

    kv_bool("digital path bit-perfect", s.bit_perfect.digital_path_bitperfect);
    kv_bool("no resampling in flight",  s.bit_perfect.no_resampling_in_flight);
    kv_bool("RT enabled",               s.bit_perfect.rt_enabled);
    kv_bool("no recent xrun",           s.bit_perfect.no_recent_xrun);
    kv_bool("volume at unity",          s.bit_perfect.volume_unity);
    kv_bool("no mismatch in flight",    s.bit_perfect.no_mismatch_in_flight);

    if (!s.bit_perfect.qualifications.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kMuted, "%s", "qualifications:");
        for (const auto& q : s.bit_perfect.qualifications) {
            ImGui::BulletText("%s", q.c_str());
        }
    }
    card_end();
}

} // namespace

void draw_pipeline_view_with_snapshot(const PipelineSnapshot& snap) {
    ImGui::TextColored(kMuted, "engine state:");
    ImGui::SameLine();
    ImGui::Text("%s", state_name(snap.engine_state));
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "  captured at:");
    ImGui::SameLine();
    const auto ms_since = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - snap.captured_at).count();
    ImGui::Text("%lld ms ago", static_cast<long long>(ms_since));
    ImGui::Spacing();

    source_card(snap);
    decoder_card(snap);
    format_match_card(snap);
    ring_card(snap);
    output_card(snap);
    device_card(snap);
    realtime_card(snap);
    verdict_card(snap);
}

void draw_pipeline_view(AppState& st) {
    PipelineSnapshot snap{};
    if (st.engine_) {
        snap = st.engine_->pipeline_snapshot();
    }
    draw_pipeline_view_with_snapshot(snap);
}

} // namespace transporter::gui
