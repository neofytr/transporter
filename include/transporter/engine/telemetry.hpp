// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_ENGINE_TELEMETRY_HPP
#define TRANSPORTER_ENGINE_TELEMETRY_HPP

#include <transporter/engine/decoder.hpp>
#include <transporter/engine/device.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/engine/rt.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace transporter::engine {

// Forward — the engine state enum lives in engine.hpp; defined there.
enum class State : std::uint8_t;

struct SourceStage {
    std::string file_path;
    std::string codec_name;            // "FLAC" / "MP3" / "WAV" / etc.
    std::string container;             // "raw" / "Ogg" / "MP4"
    std::string decoder_lib_version;   // best-effort
    std::uint32_t bitrate_kbps = 0;    // 0 for lossless
    std::uint16_t bit_depth_file = 0;  // declared bit depth
    std::uint16_t channels = 0;
    std::uint32_t sample_rate_hz = 0;
    std::uint64_t total_frames = 0;
    std::chrono::milliseconds duration{0};
    Tags tags;
};

struct DecoderStage {
    PcmFormat output_format{};
    std::uint64_t frames_produced = 0;  // session-wide
    std::string thread_state;           // "idle"|"decoding"|"blocked-ring-full"|"eof"|"error"
};

struct FormatMatchStage {
    PcmFormat declared{};
    PcmFormat matched{};
    bool matched_ok = false;
    std::string rejection_reason;       // populated on miss
    std::vector<SampleFormat> device_format_set;
    std::vector<std::uint32_t> device_rate_set;
};

struct RingStage {
    std::size_t capacity_bytes = 0;
    std::size_t fill_bytes = 0;
    std::size_t fill_frames = 0;
    std::chrono::microseconds fill_us{0};       // fill_frames / sample_rate
    std::size_t max_watermark_bytes = 0;        // session-wide
};

struct OutputStage {
    std::uint32_t period_size_frames = 0;
    std::uint32_t periods = 0;
    std::uint32_t buffer_size_frames = 0;
    PcmFormat hw_params_set{};
    std::uint64_t frames_written = 0;           // session-wide
    std::uint32_t xrun_count = 0;               // session-wide
};

struct DeviceStage {
    DeviceFingerprint fingerprint;
    DeviceCapabilities capabilities;
    std::string current_hw_string;
    // Hotplug state. is_connected reflects whether the engine has the
    // active DAC open right now; false while in Disconnected. The
    // timestamp is the last steady_clock::now() at which a Removed event
    // matched the active fingerprint; default-constructed (zero TP) when
    // the engine has never observed a disconnect this session.
    bool is_connected = true;
    std::chrono::steady_clock::time_point last_disconnected_at{};
};

struct RealtimeStage {
    rt::Status status;
    std::uint64_t trace_dropped = 0;
};

// Bit-perfect verdict per the locked spec. Three-state with per-condition
// breakdown so the GUI can render a tooltip explaining any caveat.
struct BitPerfectVerdict {
    enum class Level : std::uint8_t { Yes, Qualified, No };

    Level level = Level::No;
    bool digital_path_bitperfect = false;
    bool no_resampling_in_flight = false;
    bool rt_enabled = false;
    bool no_recent_xrun = false;
    bool volume_unity = false;
    bool no_mismatch_in_flight = false;
    std::vector<std::string> qualifications;  // human-readable per-condition reasons
};

struct PipelineSnapshot {
    State engine_state;  // requires <transporter/engine/engine.hpp> at use site
    SourceStage source;
    DecoderStage decoder;
    FormatMatchStage format_match;
    RingStage ring;
    OutputStage output;
    DeviceStage device;
    RealtimeStage realtime;
    BitPerfectVerdict bit_perfect;
    std::chrono::steady_clock::time_point captured_at;
};

} // namespace transporter::engine

#endif
