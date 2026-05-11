// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapshot_serialize.hpp"

#include <transporter/engine/device.hpp>
#include <transporter/engine/engine.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/engine/rt.hpp>
#include <transporter/engine/telemetry.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace transporter::dbus_svc {

namespace {

const char* engine_state_to_str(transporter::engine::State s) {
    using S = transporter::engine::State;
    switch (s) {
    case S::Idle:         return "Idle";
    case S::Loading:      return "Loading";
    case S::Playing:      return "Playing";
    case S::Paused:       return "Paused";
    case S::Stopped:      return "Stopped";
    case S::Error:        return "Error";
    case S::Disconnected: return "Disconnected";
    }
    return "?";
}

std::string format_pcm(const transporter::engine::PcmFormat& f) {
    if (f.sample_rate_hz == 0 && f.channels == 0) {
        return {};
    }
    std::string out = std::string{transporter::engine::sample_format_name(
        f.sample_format)};
    out.push_back('@');
    out.append(std::to_string(f.sample_rate_hz));
    out.push_back('x');
    out.append(std::to_string(f.channels));
    return out;
}

std::vector<std::string> sample_formats_to_strings(
    const std::vector<transporter::engine::SampleFormat>& formats) {
    std::vector<std::string> out;
    out.reserve(formats.size());
    for (auto f : formats) {
        out.emplace_back(transporter::engine::sample_format_name(f));
    }
    return out;
}

} // namespace

std::string usb_vid_pid_serial(
    const transporter::engine::DeviceFingerprint& fp) {
    if (!fp.is_usb || fp.usb_vendor_id.empty()) {
        return {};
    }
    std::string out;
    out.reserve(fp.usb_vendor_id.size() + fp.usb_product_id.size() +
                fp.usb_serial.size() + 2);
    out.append(fp.usb_vendor_id);
    out.push_back(':');
    out.append(fp.usb_product_id);
    out.push_back(':');
    out.append(fp.usb_serial);  // empty serial is fine; "vid:pid:" is signal
    return out;
}

std::vector<std::string> device_format_strings(
    const transporter::engine::DeviceCapabilities& caps) {
    std::vector<std::string> out;
    out.reserve(caps.formats.size() * caps.sample_rates.size());
    for (auto f : caps.formats) {
        for (auto r : caps.sample_rates) {
            std::string s{transporter::engine::sample_format_name(f)};
            s.push_back('@');
            s.append(std::to_string(r));
            out.emplace_back(std::move(s));
        }
    }
    return out;
}

std::map<std::string, std::map<std::string, sdbus::Variant>>
snapshot_to_dict(const transporter::engine::PipelineSnapshot& snap) {
    std::map<std::string, std::map<std::string, sdbus::Variant>> out;

    // source
    {
        std::map<std::string, sdbus::Variant> m;
        m.emplace("file_path", sdbus::Variant{snap.source.file_path});
        m.emplace("codec", sdbus::Variant{snap.source.codec_name});
        m.emplace("container", sdbus::Variant{snap.source.container});
        m.emplace("decoder_lib",
                  sdbus::Variant{snap.source.decoder_lib_version});
        m.emplace("bitrate_kbps",
                  sdbus::Variant{snap.source.bitrate_kbps});
        m.emplace("bit_depth",
                  sdbus::Variant{snap.source.bit_depth_file});
        m.emplace("channels", sdbus::Variant{snap.source.channels});
        m.emplace("sample_rate_hz",
                  sdbus::Variant{snap.source.sample_rate_hz});
        m.emplace("total_frames",
                  sdbus::Variant{snap.source.total_frames});
        m.emplace("duration_ms",
                  sdbus::Variant{static_cast<std::int64_t>(
                      snap.source.duration.count())});
        m.emplace("title", sdbus::Variant{snap.source.tags.title});
        m.emplace("artist", sdbus::Variant{snap.source.tags.artist});
        m.emplace("album_artist", sdbus::Variant{snap.source.tags.album_artist});
        m.emplace("album", sdbus::Variant{snap.source.tags.album});
        m.emplace("track_no", sdbus::Variant{snap.source.tags.track_no});
        m.emplace("date", sdbus::Variant{snap.source.tags.date});
        out.emplace("source", std::move(m));
    }

    // decoder
    {
        std::map<std::string, sdbus::Variant> m;
        m.emplace("format", sdbus::Variant{format_pcm(snap.decoder.output_format)});
        m.emplace("frames_produced",
                  sdbus::Variant{snap.decoder.frames_produced});
        m.emplace("thread_state",
                  sdbus::Variant{snap.decoder.thread_state});
        out.emplace("decoder", std::move(m));
    }

    // format_match
    {
        std::map<std::string, sdbus::Variant> m;
        m.emplace("declared",
                  sdbus::Variant{format_pcm(snap.format_match.declared)});
        m.emplace("matched",
                  sdbus::Variant{format_pcm(snap.format_match.matched)});
        m.emplace("matched_ok",
                  sdbus::Variant{snap.format_match.matched_ok});
        m.emplace("rejection_reason",
                  sdbus::Variant{snap.format_match.rejection_reason});
        m.emplace("device_formats",
                  sdbus::Variant{sample_formats_to_strings(
                      snap.format_match.device_format_set)});
        m.emplace("device_rates",
                  sdbus::Variant{snap.format_match.device_rate_set});
        out.emplace("format_match", std::move(m));
    }

    // ring
    {
        std::map<std::string, sdbus::Variant> m;
        m.emplace("capacity_bytes",
                  sdbus::Variant{static_cast<std::uint64_t>(
                      snap.ring.capacity_bytes)});
        m.emplace("fill_bytes",
                  sdbus::Variant{static_cast<std::uint64_t>(
                      snap.ring.fill_bytes)});
        m.emplace("fill_frames",
                  sdbus::Variant{static_cast<std::uint64_t>(
                      snap.ring.fill_frames)});
        m.emplace("fill_us",
                  sdbus::Variant{static_cast<std::uint32_t>(
                      snap.ring.fill_us.count())});
        m.emplace("max_watermark_bytes",
                  sdbus::Variant{static_cast<std::uint64_t>(
                      snap.ring.max_watermark_bytes)});
        out.emplace("ring", std::move(m));
    }

    // output
    {
        std::map<std::string, sdbus::Variant> m;
        m.emplace("period_size_frames",
                  sdbus::Variant{snap.output.period_size_frames});
        m.emplace("periods", sdbus::Variant{snap.output.periods});
        m.emplace("buffer_size_frames",
                  sdbus::Variant{snap.output.buffer_size_frames});
        m.emplace("hw_format",
                  sdbus::Variant{format_pcm(snap.output.hw_params_set)});
        m.emplace("frames_written",
                  sdbus::Variant{snap.output.frames_written});
        m.emplace("frames_written_at_track_start",
                  sdbus::Variant{snap.output.frames_written_at_track_start});
        m.emplace("xrun_count",
                  sdbus::Variant{snap.output.xrun_count});
        out.emplace("output", std::move(m));
    }

    // device
    {
        std::map<std::string, sdbus::Variant> m;
        m.emplace("hw_string",
                  sdbus::Variant{snap.device.current_hw_string});
        m.emplace("alsa_card_name",
                  sdbus::Variant{snap.device.fingerprint.alsa_card_name});
        m.emplace("alsa_card_longname",
                  sdbus::Variant{snap.device.fingerprint.alsa_card_longname});
        m.emplace("is_usb",
                  sdbus::Variant{snap.device.fingerprint.is_usb});
        m.emplace("usb_vid_pid_serial",
                  sdbus::Variant{usb_vid_pid_serial(snap.device.fingerprint)});
        m.emplace("channels_min",
                  sdbus::Variant{snap.device.capabilities.channels_min});
        m.emplace("channels_max",
                  sdbus::Variant{snap.device.capabilities.channels_max});
        m.emplace("is_connected",
                  sdbus::Variant{snap.device.is_connected});
        out.emplace("device", std::move(m));
    }

    // realtime
    {
        std::map<std::string, sdbus::Variant> m;
        const auto& st = snap.realtime.status;
        const std::string mode_str =
            st.mode == transporter::engine::rt::Mode::Fifo ? "FIFO" : "OTHER";
        m.emplace("mode", sdbus::Variant{mode_str});
        m.emplace("priority",
                  sdbus::Variant{static_cast<std::int32_t>(st.priority)});
        m.emplace("memlocked", sdbus::Variant{st.memlocked});
        m.emplace("cpu_pinned",
                  sdbus::Variant{static_cast<std::int32_t>(st.cpu_pinned)});
        m.emplace("fallback_reason",
                  sdbus::Variant{st.fallback_reason});
        m.emplace("trace_dropped",
                  sdbus::Variant{snap.realtime.trace_dropped});
        out.emplace("realtime", std::move(m));
    }

    // bit_perfect
    {
        std::map<std::string, sdbus::Variant> m;
        m.emplace("level",
                  sdbus::Variant{
                      std::string{bit_perfect_level_name(snap.bit_perfect.level)}});
        m.emplace("digital_path_bitperfect",
                  sdbus::Variant{snap.bit_perfect.digital_path_bitperfect});
        m.emplace("no_resampling_in_flight",
                  sdbus::Variant{snap.bit_perfect.no_resampling_in_flight});
        m.emplace("rt_enabled",
                  sdbus::Variant{snap.bit_perfect.rt_enabled});
        m.emplace("no_recent_xrun",
                  sdbus::Variant{snap.bit_perfect.no_recent_xrun});
        m.emplace("volume_unity",
                  sdbus::Variant{snap.bit_perfect.volume_unity});
        m.emplace("no_mismatch_in_flight",
                  sdbus::Variant{snap.bit_perfect.no_mismatch_in_flight});
        m.emplace("qualifications",
                  sdbus::Variant{snap.bit_perfect.qualifications});
        out.emplace("bit_perfect", std::move(m));
    }

    // engine_state — exposed flat via the property; included here for the
    // single-shot consumer that wants everything in one round trip.
    {
        std::map<std::string, sdbus::Variant> m;
        m.emplace("state",
                  sdbus::Variant{
                      std::string{engine_state_to_str(snap.engine_state)}});
        out.emplace("engine", std::move(m));
    }

    return out;
}

} // namespace transporter::dbus_svc
