// SPDX-License-Identifier: GPL-3.0-or-later
//
// org.mpris.MediaPlayer2.transporter — custom interface for power users and
// scripts. Mirrors PipelineSnapshot. Read-only properties are direct
// flattenings; GetSnapshot returns the same data as a structured dict.
//
// GetSnapshot reply schema (signature: a{sa{sv}}):
//
//   "source"        -> {
//       "file_path":         s    file path or empty
//       "codec":             s    "FLAC" / "MP3" / "WAV" / ...
//       "container":         s
//       "decoder_lib":       s
//       "bitrate_kbps":      u
//       "bit_depth":         q    declared bit depth from the file
//       "channels":          q
//       "sample_rate_hz":    u
//       "total_frames":      t
//       "duration_ms":       x    int64 milliseconds
//       "title":             s
//       "artist":            s
//       "album_artist":      s
//       "album":             s
//       "track_no":          s
//       "date":              s
//   }
//   "decoder"       -> {
//       "format":            s    "S16_LE@44100x2"-style
//       "frames_produced":   t
//       "thread_state":      s
//   }
//   "format_match"  -> {
//       "declared":          s
//       "matched":           s
//       "matched_ok":        b
//       "rejection_reason":  s
//       "device_formats":    as   ["S16_LE","S24_LE",...]
//       "device_rates":      au
//   }
//   "ring"          -> {
//       "capacity_bytes":        t
//       "fill_bytes":            t
//       "fill_frames":           t
//       "fill_us":               u
//       "max_watermark_bytes":   t
//   }
//   "output"        -> {
//       "period_size_frames":    u
//       "periods":               u
//       "buffer_size_frames":    u
//       "hw_format":             s   "S24_LE@96000x2"
//       "frames_written":        t
//       "frames_written_at_track_start": t
//       "xrun_count":            u
//   }
//   "device"        -> {
//       "hw_string":             s   "hw:CARD=...,DEV=N"
//       "alsa_card_name":        s
//       "alsa_card_longname":    s
//       "is_usb":                b
//       "usb_vid_pid_serial":    s   "vid:pid:serial" or empty
//       "channels_min":          q
//       "channels_max":          q
//       "is_connected":          b
//   }
//   "realtime"      -> {
//       "mode":                  s   "FIFO" | "OTHER"
//       "priority":              i
//       "memlocked":             b
//       "cpu_pinned":            i
//       "fallback_reason":       s
//       "trace_dropped":         t
//   }
//   "bit_perfect"   -> {
//       "level":                 s   "YES" | "QUALIFIED" | "NO"
//       "digital_path_bitperfect": b
//       "no_resampling_in_flight": b
//       "rt_enabled":              b
//       "no_recent_xrun":          b
//       "volume_unity":            b
//       "no_mismatch_in_flight":   b
//       "qualifications":          as
//   }

#include "transporter_iface.hpp"
#include "snapshot_serialize.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/telemetry.hpp>
#include <transporter/library/library.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace transporter::dbus_svc {

namespace {

constexpr const char* kIface = "org.mpris.MediaPlayer2.transporter";

const char* engine_state_name(transporter::engine::State s) {
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

} // namespace

TransporterIface::TransporterIface(sdbus::IObject& obj,
                                   transporter::engine::Engine* engine,
                                   transporter::library::Library* library,
                                   ReloadHook reload,
                                   RescanHook rescan)
    : obj_(obj),
      engine_(engine),
      library_(library),
      reload_(std::move(reload)),
      rescan_(std::move(rescan)) {
    register_vtable();
}

void TransporterIface::emit_bitperfect_changed(
    const std::string& level,
    const std::vector<std::string>& qualifications) {
    try {
        obj_.emitSignal("BitPerfectChanged")
            .onInterface(kIface)
            .withArguments(level, qualifications);
    } catch (const sdbus::Error&) {
        // best-effort; ignore
    }
}

void TransporterIface::register_vtable() {
    auto snap = [this] {
        return engine_ ? engine_->pipeline_snapshot()
                       : transporter::engine::PipelineSnapshot{};
    };

    obj_.addVTable(
        // Methods
        sdbus::registerMethod("GetSnapshot")
            .withOutputParamNames("Snapshot")
            .implementedAs([this] -> std::map<std::string,
                                              std::map<std::string,
                                                       sdbus::Variant>> {
                if (engine_ == nullptr) {
                    return snapshot_to_dict(
                        transporter::engine::PipelineSnapshot{});
                }
                return snapshot_to_dict(engine_->pipeline_snapshot());
            }),
        sdbus::registerMethod("ReloadConfig")
            .withOutputParamNames("Applied")
            .implementedAs([this] -> bool {
                if (!reload_) {
                    throw sdbus::Error(
                        sdbus::Error::Name{
                            "org.mpris.MediaPlayer2.transporter.Error.NotSupported"},
                        "ReloadConfig hook not wired");
                }
                return reload_();
            }),
        sdbus::registerMethod("RescanLibrary").implementedAs([this] {
            if (rescan_) {
                rescan_();
            } else if (library_) {
                library_->rescan_async();
            } else {
                throw sdbus::Error(
                    sdbus::Error::Name{
                        "org.mpris.MediaPlayer2.transporter.Error.NotSupported"},
                    "library not attached");
            }
        }),
        sdbus::registerMethod("SelectDevice")
            .withInputParamNames("HwString")
            .implementedAs([](const std::string& /*hw*/) {
                throw sdbus::Error(
                    sdbus::Error::Name{
                        "org.mpris.MediaPlayer2.transporter.Error.NotImplemented"},
                    "SelectDevice is reserved for Phase 12");
            }),
        // Signal
        sdbus::registerSignal("BitPerfectChanged")
            .withParameters<std::string, std::vector<std::string>>(
                {"Level", "Qualifications"}),
        // Properties (read-only flattenings).
        sdbus::registerProperty("BitPerfectLevel").withGetter([snap] {
            return std::string{
                bit_perfect_level_name(snap().bit_perfect.level)};
        }),
        sdbus::registerProperty("BitPerfectQualifications").withGetter([snap] {
            return snap().bit_perfect.qualifications;
        }),
        sdbus::registerProperty("RtMode").withGetter([snap] {
            using M = transporter::engine::rt::Mode;
            return std::string{
                snap().realtime.status.mode == M::Fifo ? "FIFO" : "OTHER"};
        }),
        sdbus::registerProperty("RtPriority").withGetter([snap] {
            return static_cast<std::int32_t>(snap().realtime.status.priority);
        }),
        sdbus::registerProperty("XrunCount").withGetter([snap] {
            return static_cast<std::uint32_t>(snap().output.xrun_count);
        }),
        sdbus::registerProperty("RingFillUs").withGetter([snap] {
            return static_cast<std::uint32_t>(snap().ring.fill_us.count());
        }),
        sdbus::registerProperty("RingMaxWatermarkBytes").withGetter([snap] {
            return static_cast<std::uint64_t>(snap().ring.max_watermark_bytes);
        }),
        sdbus::registerProperty("CurrentDeviceHw").withGetter([snap] {
            return snap().device.current_hw_string;
        }),
        sdbus::registerProperty("CurrentDeviceCardName").withGetter([snap] {
            return snap().device.fingerprint.alsa_card_name;
        }),
        sdbus::registerProperty("CurrentDeviceUsbVidPidSerial").withGetter(
            [snap] { return usb_vid_pid_serial(snap().device.fingerprint); }),
        sdbus::registerProperty("DeviceFormatsSupported").withGetter([snap] {
            return device_format_strings(snap().device.capabilities);
        }),
        sdbus::registerProperty("MatchedSampleRateHz").withGetter([snap] {
            return static_cast<std::uint32_t>(
                snap().format_match.matched.sample_rate_hz);
        }),
        sdbus::registerProperty("MatchedSampleFormat").withGetter([snap] {
            return std::string{transporter::engine::sample_format_name(
                snap().format_match.matched.sample_format)};
        }),
        sdbus::registerProperty("MatchedChannels").withGetter([snap] {
            return static_cast<std::uint32_t>(
                snap().format_match.matched.channels);
        }),
        sdbus::registerProperty("EngineState").withGetter([snap] {
            return std::string{engine_state_name(snap().engine_state)};
        })
    ).forInterface(sdbus::InterfaceName{kIface});
}

} // namespace transporter::dbus_svc
