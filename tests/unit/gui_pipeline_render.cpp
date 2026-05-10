// SPDX-License-Identifier: GPL-3.0-or-later
//
// Headless ImGui render of the Pipeline view against a hand-built
// PipelineSnapshot. ImGui's null platform backend lets us render a frame
// without a window. We assert that strings we expect (e.g. "FLAC", "YES",
// the hw: device string) appear in the resulting glyph stream.
//
// This is brittle but cheap: it catches plumbing regressions in the
// PipelineView -> ImGui bridge.

#include "../../src/gui/views/app.hpp"

#include <transporter/engine/device.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/engine/rt.hpp>
#include <transporter/engine/telemetry.hpp>

#include <imgui.h>

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace tg = transporter::gui;
namespace te = transporter::engine;

namespace {

int fail(const char* where) {
    std::fprintf(stderr, "FAIL %s\n", where);
    return 1;
}

te::PipelineSnapshot make_snapshot() {
    te::PipelineSnapshot s{};
    s.engine_state = te::State::Playing;
    s.captured_at = std::chrono::steady_clock::now();

    s.source.file_path = "/srv/music/Aria Black/Origin/01-spark.flac";
    s.source.codec_name = "FLAC";
    s.source.container = "raw";
    s.source.decoder_lib_version = "libFLAC 1.4.3";
    s.source.bitrate_kbps = 0;
    s.source.bit_depth_file = 16;
    s.source.channels = 2;
    s.source.sample_rate_hz = 44100;
    s.source.total_frames = 5'000'000;
    s.source.duration = std::chrono::milliseconds(113378);
    s.source.tags.artist = "Aria Black";
    s.source.tags.album = "Origin";
    s.source.tags.title = "Spark";
    s.source.tags.track_no = "1";
    s.source.tags.date = "2024";

    s.decoder.output_format = {44100, 2, te::SampleFormat::S16_LE};
    s.decoder.frames_produced = 12345;
    s.decoder.thread_state = "decoding";

    s.format_match.declared = {44100, 2, te::SampleFormat::S16_LE};
    s.format_match.matched = {44100, 2, te::SampleFormat::S16_LE};
    s.format_match.matched_ok = true;
    s.format_match.device_format_set = {te::SampleFormat::S16_LE,
                                        te::SampleFormat::S24_3LE,
                                        te::SampleFormat::S32_LE};
    s.format_match.device_rate_set = {44100, 48000, 88200, 96000, 192000};

    s.ring.capacity_bytes = 1u << 20;
    s.ring.fill_bytes = 8192;
    s.ring.fill_frames = 2048;
    s.ring.fill_us = std::chrono::microseconds(46439);
    s.ring.max_watermark_bytes = 32768;

    s.output.period_size_frames = 528;
    s.output.periods = 4;
    s.output.buffer_size_frames = 2112;
    s.output.hw_params_set = {44100, 2, te::SampleFormat::S16_LE};
    s.output.frames_written = 22050;
    s.output.xrun_count = 0;

    s.device.fingerprint.alsa_card_name = "USBDAC";
    s.device.fingerprint.alsa_card_longname = "Topping E70 Velvet";
    s.device.fingerprint.is_usb = true;
    s.device.fingerprint.usb_vendor_id = "152a";
    s.device.fingerprint.usb_product_id = "8750";
    s.device.fingerprint.usb_serial = "TPS-001";
    s.device.current_hw_string = "hw:CARD=USBDAC,DEV=0";
    s.device.is_connected = true;
    s.device.capabilities.formats = {te::SampleFormat::S16_LE,
                                     te::SampleFormat::S24_3LE,
                                     te::SampleFormat::S32_LE};
    s.device.capabilities.sample_rates = {44100, 48000, 88200, 96000, 192000};
    s.device.capabilities.channels_min = 1;
    s.device.capabilities.channels_max = 2;
    s.device.capabilities.hw_volume.present = true;
    s.device.capabilities.hw_volume.has_db_scale = true;
    s.device.capabilities.hw_volume.min_db_x100 = -6000;
    s.device.capabilities.hw_volume.max_db_x100 = 0;
    s.device.capabilities.hw_volume.element_name = "PCM";

    s.realtime.status.mode = te::rt::Mode::Fifo;
    s.realtime.status.priority = 80;
    s.realtime.status.memlocked = true;
    s.realtime.status.cpu_pinned = 7;
    s.realtime.status.fallback_reason = "";
    s.realtime.trace_dropped = 0;

    using L = te::BitPerfectVerdict::Level;
    s.bit_perfect.level = L::Yes;
    s.bit_perfect.digital_path_bitperfect = true;
    s.bit_perfect.no_resampling_in_flight = true;
    s.bit_perfect.rt_enabled = true;
    s.bit_perfect.no_recent_xrun = true;
    s.bit_perfect.volume_unity = true;
    s.bit_perfect.no_mismatch_in_flight = true;
    return s;
}

// ImGui::LogToFile redirects every Text/TextUnformatted/etc. call into a
// file. We point it at a unique tempfile, render the view, finalize the log,
// then read the file back as a string.
std::string render_pipeline_to_log(const te::PipelineSnapshot& snap) {
    ImGui::SetCurrentContext(ImGui::CreateContext());
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    // Default font, null backend rendererflag, no actual GL.
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsAlpha8(&px, &w, &h);
    io.Fonts->TexID = static_cast<ImTextureID>(0xdead);
    io.DisplaySize = ImVec2(1280.0f, 1024.0f);
    io.DeltaTime = 1.0f / 60.0f;

    char tmp[] = "/tmp/transporter_imgui_log_XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) {
        ImGui::DestroyContext();
        return {};
    }
    ::close(fd);

    ImGui::NewFrame();
    ImGui::Begin("test");
    ImGui::LogToFile(-1, tmp);
    transporter::gui::draw_pipeline_view_with_snapshot(snap);
    ImGui::LogFinish();
    ImGui::End();
    ImGui::EndFrame();
    ImGui::Render();

    std::string out;
    if (FILE* f = std::fopen(tmp, "rb"); f != nullptr) {
        char buf[4096];
        for (;;) {
            const std::size_t n = std::fread(buf, 1, sizeof(buf), f);
            if (n == 0) break;
            out.append(buf, n);
        }
        std::fclose(f);
    }
    std::remove(tmp);

    ImGui::DestroyContext();
    return out;
}

} // namespace

int main() {
    const auto snap = make_snapshot();
    const std::string text = render_pipeline_to_log(snap);

    struct Probe { const char* needle; const char* label; };
    constexpr Probe probes[] = {
        {"FLAC",                 "Source.codec"},
        {"libFLAC 1.4.3",        "Source.decoder_lib"},
        {"Aria Black",           "Source.tag.artist"},
        {"Spark",                "Source.tag.title"},
        {"Origin",               "Source.tag.album"},
        {"hw:CARD=USBDAC,DEV=0", "Device.hw_string"},
        {"Topping E70 Velvet",   "Device.longname"},
        {"152a:8750",            "Device.usb id"},
        {"TPS-001",              "Device.usb serial"},
        {"S16_LE",               "format string"},
        {"YES",                  "Verdict tag"},
        {"SCHED_FIFO",           "Realtime.policy"},
        {"PCM",                  "HW volume element"},
        {"Source",               "card header"},
        {"Decoder",              "card header"},
        {"Format match",         "card header"},
        {"Ring",                 "card header"},
        {"Output",               "card header"},
        {"Device",               "card header"},
        {"Realtime",             "card header"},
        {"Bit-perfect verdict",  "card header"},
    };

    int errors = 0;
    for (const auto& p : probes) {
        if (text.find(p.needle) == std::string::npos) {
            std::fprintf(stderr, "FAIL %s: needle=%s not found\n",
                         p.label, p.needle);
            ++errors;
        }
    }
    if (errors != 0) {
        std::fputs("---- captured text (truncated) ----\n", stderr);
        const std::size_t take = std::min<std::size_t>(text.size(), 4096);
        std::fwrite(text.data(), 1, take, stderr);
        std::fputc('\n', stderr);
        return fail("probes missing");
    }
    std::puts("ok gui_pipeline_render");
    return 0;
}
