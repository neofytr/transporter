// SPDX-License-Identifier: GPL-3.0-or-later
//
// org.mpris.MediaPlayer2.Player. Transport methods + the property surface
// MPRIS clients (playerctl, status bars, hyprland osd) expect.
//
// Coverage:
//   PlayPause  - implemented, toggles via Engine::play / Engine::pause
//   Play       - implemented
//   Pause      - implemented
//   Stop       - implemented
//   Next       - implemented (delegates to caller-supplied hook)
//   Previous   - implemented (delegates to caller-supplied hook)
//   OpenUri    - implemented (file:// URIs only)
//   Seek       - implemented, engine::seek() takes absolute frame offset
//   SetPosition - implemented
//
// Properties:
//   PlaybackStatus, Metadata, Position, Volume, Rate, MinimumRate,
//   MaximumRate, Shuffle, LoopStatus, CanGoNext, CanGoPrevious, CanPlay,
//   CanPause, CanSeek, CanControl. Volume maps to the ALSA hardware mixer
//   when available; falls back to 1.0 read-only when no mixer control exists.
//
// Threading: getters and method handlers fire on the DBus event-loop thread.
// They read engine::State / pipeline_snapshot() (both lock-free or briefly
// mutexed) and call engine command-post methods (load/play/pause/stop) which
// return immediately. No DBus handler ever blocks the audio path.

#include "mpris_player.hpp"
#include "mpris_metadata.hpp"

#include <transporter/engine/device.hpp>
#include <transporter/engine/engine.hpp>
#include <transporter/engine/telemetry.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace transporter::dbus_svc {

namespace {

constexpr const char* kIface = "org.mpris.MediaPlayer2.Player";

// Convert a "file:///abs/path" URI into a filesystem path. Returns empty path
// on any malformed input. Only file:// is supported (matches
// SupportedUriSchemes).
std::filesystem::path parse_file_uri(const std::string& uri) {
    static constexpr std::string_view kPrefix = "file://";
    if (uri.size() < kPrefix.size() ||
        uri.compare(0, kPrefix.size(), kPrefix) != 0) {
        return {};
    }
    // Skip optional host segment ("file://localhost/..."); MPRIS clients
    // typically emit "file:///abs/path".
    std::size_t off = kPrefix.size();
    if (off < uri.size() && uri[off] != '/') {
        const auto slash = uri.find('/', off);
        if (slash == std::string::npos) {
            return {};
        }
        off = slash;
    }
    // No percent-decoding here; modern playerctl emits unencoded paths in
    // common-case scenarios. If decoding is needed later, add it locally
    // rather than pulling in a URI library.
    return std::filesystem::path{uri.substr(off)};
}

} // namespace

MprisPlayer::MprisPlayer(sdbus::IObject& obj,
                         transporter::engine::Engine* engine,
                         NextHook next,
                         PreviousHook prev,
                         LoadHook load,
                         ShuffleGetter shuffle_get,
                         ShuffleSetter shuffle_set,
                         LoopStatusGetter loop_get,
                         LoopStatusSetter loop_set)
    : obj_(obj),
      engine_(engine),
      next_hook_(std::move(next)),
      prev_hook_(std::move(prev)),
      load_hook_(std::move(load)),
      shuffle_get_(std::move(shuffle_get)),
      shuffle_set_(std::move(shuffle_set)),
      loop_get_(std::move(loop_get)),
      loop_set_(std::move(loop_set)) {
    register_vtable();
}

std::string MprisPlayer::playback_status_for(transporter::engine::State s) {
    using S = transporter::engine::State;
    switch (s) {
    case S::Playing:
        return "Playing";
    case S::Paused:
        return "Paused";
    case S::Idle:
    case S::Loading:
    case S::Stopped:
    case S::Error:
    case S::Disconnected:
        return "Stopped";
    }
    return "Stopped";
}

std::map<std::string, sdbus::Variant>
MprisPlayer::build_metadata_locked() const {
    if (engine_ == nullptr) {
        std::map<std::string, sdbus::Variant> out;
        out.emplace("mpris:trackid",
                    sdbus::Variant{sdbus::ObjectPath{
                        "/org/mpris/MediaPlayer2/transporter/track/0"}});
        return out;
    }
    const auto snap = engine_->pipeline_snapshot();
    return mpris_metadata_from_snapshot(
        snap, "/org/mpris/MediaPlayer2/transporter/track/" +
                  std::to_string(track_counter_));
}

void MprisPlayer::refresh_metadata() {
    {
        std::lock_guard lk(meta_mtx_);
        ++track_counter_;
        metadata_ = build_metadata_locked();
    }
    try {
        obj_.emitPropertiesChangedSignal(kIface, {sdbus::PropertyName{"Metadata"}});
    } catch (const sdbus::Error&) {
        // Most likely the connection is being torn down; ignore.
    }
}

void MprisPlayer::notify_state(transporter::engine::State s) {
    const std::string status = playback_status_for(s);
    bool changed = false;
    {
        std::lock_guard lk(meta_mtx_);
        if (status != last_status_) {
            last_status_ = status;
            changed = true;
        }
    }
    if (changed) {
        try {
            obj_.emitPropertiesChangedSignal(
                kIface, {sdbus::PropertyName{"PlaybackStatus"}});
        } catch (const sdbus::Error&) {
            // ignore
        }
    }
}

void MprisPlayer::notify_shuffle() {
    try {
        obj_.emitPropertiesChangedSignal(kIface, {sdbus::PropertyName{"Shuffle"}});
    } catch (const sdbus::Error&) {}
}

void MprisPlayer::notify_loop_status() {
    try {
        obj_.emitPropertiesChangedSignal(kIface, {sdbus::PropertyName{"LoopStatus"}});
    } catch (const sdbus::Error&) {}
}

void MprisPlayer::notify_volume() {
    try {
        obj_.emitPropertiesChangedSignal(kIface, {sdbus::PropertyName{"Volume"}});
    } catch (const sdbus::Error&) {}
}

void MprisPlayer::register_vtable() {
    auto post_play = [this] {
        if (engine_) {
            (void)engine_->play();
        }
    };
    auto post_pause = [this] {
        if (engine_) {
            (void)engine_->pause();
        }
    };
    auto post_stop = [this] {
        if (engine_) {
            (void)engine_->stop();
        }
    };

    obj_.addVTable(
        // Methods
        sdbus::registerMethod("Next").implementedAs([this] {
            if (next_hook_) {
                if (auto p = next_hook_(); p && load_hook_) {
                    load_hook_(*p);
                }
            }
        }),
        sdbus::registerMethod("Previous").implementedAs([this] {
            if (prev_hook_) {
                if (auto p = prev_hook_(); p && load_hook_) {
                    load_hook_(*p);
                }
            }
        }),
        sdbus::registerMethod("Pause").implementedAs(post_pause),
        sdbus::registerMethod("PlayPause").implementedAs(
            [this, post_play, post_pause] {
                if (engine_ == nullptr) {
                    return;
                }
                if (engine_->state() == transporter::engine::State::Playing) {
                    post_pause();
                } else {
                    post_play();
                }
            }),
        sdbus::registerMethod("Stop").implementedAs(post_stop),
        sdbus::registerMethod("Play").implementedAs(post_play),
        sdbus::registerMethod("Seek")
            .withInputParamNames("Offset")
            .implementedAs([this](std::int64_t offset_us) {
                if (engine_ == nullptr) {
                    return;
                }
                const auto snap = engine_->pipeline_snapshot();
                const std::uint32_t rate = snap.source.sample_rate_hz;
                if (rate == 0) {
                    return;
                }
                const std::int64_t cur_frame =
                    static_cast<std::int64_t>(snap.output.frames_written) -
                    static_cast<std::int64_t>(snap.output.frames_written_at_track_start);
                const std::int64_t delta_frames =
                    (offset_us * static_cast<std::int64_t>(rate)) / 1'000'000LL;
                const std::int64_t target =
                    std::max(std::int64_t{0}, cur_frame + delta_frames);
                (void)engine_->seek(static_cast<std::uint64_t>(target));
                // Emit Seeked with estimated new position in microseconds.
                const std::int64_t new_pos_us =
                    (target * 1'000'000LL) / static_cast<std::int64_t>(rate);
                try {
                    obj_.emitSignal(sdbus::SignalName{"Seeked"})
                        .onInterface(sdbus::InterfaceName{kIface})
                        .withArguments(new_pos_us);
                } catch (const sdbus::Error&) {}
            }),
        sdbus::registerMethod("SetPosition")
            .withInputParamNames("TrackId", "Position")
            .implementedAs([this](const sdbus::ObjectPath& /*tid*/, std::int64_t pos_us) {
                if (engine_ == nullptr) {
                    return;
                }
                const auto snap = engine_->pipeline_snapshot();
                const std::uint32_t rate = snap.source.sample_rate_hz;
                if (rate == 0 || pos_us < 0) {
                    return;
                }
                const std::uint64_t frame =
                    (static_cast<std::uint64_t>(pos_us) * rate) / 1'000'000ULL;
                (void)engine_->seek(frame);
                try {
                    obj_.emitSignal(sdbus::SignalName{"Seeked"})
                        .onInterface(sdbus::InterfaceName{kIface})
                        .withArguments(pos_us);
                } catch (const sdbus::Error&) {}
            }),
        sdbus::registerMethod("OpenUri")
            .withInputParamNames("Uri")
            .implementedAs([this](const std::string& uri) {
                const auto path = parse_file_uri(uri);
                if (path.empty()) {
                    throw sdbus::Error(
                        sdbus::Error::Name{"org.mpris.MediaPlayer2.Player.Error.InvalidUri"},
                        "Only file:// URIs are supported");
                }
                if (load_hook_) {
                    load_hook_(path);
                } else if (engine_) {
                    (void)engine_->load(path);
                    (void)engine_->play();
                }
            }),
        // Properties
        sdbus::registerProperty("PlaybackStatus").withGetter([this] {
            std::lock_guard lk(meta_mtx_);
            if (last_status_.empty()) {
                return playback_status_for(
                    engine_ ? engine_->state()
                            : transporter::engine::State::Idle);
            }
            return last_status_;
        }),
        sdbus::registerProperty("LoopStatus")
            .withGetter([this] {
                return loop_get_ ? loop_get_() : std::string{"None"};
            })
            .withSetter([this](const std::string& v) {
                if (loop_set_) {
                    loop_set_(v);
                }
            }),
        sdbus::registerProperty("Rate").withGetter([] { return 1.0; }),
        sdbus::registerProperty("Shuffle")
            .withGetter([this] { return shuffle_get_ ? shuffle_get_() : false; })
            .withSetter([this](const bool v) {
                if (shuffle_set_) {
                    shuffle_set_(v);
                }
            }),
        sdbus::registerProperty("Metadata").withGetter([this] {
            std::lock_guard lk(meta_mtx_);
            if (metadata_.empty()) {
                return build_metadata_locked();
            }
            return metadata_;
        }),
        sdbus::registerProperty("Volume")
            .withGetter([this] {
                if (engine_ == nullptr) {
                    return 1.0;
                }
                const auto& hw = engine_->pipeline_snapshot().device.current_hw_string;
                if (hw.empty()) {
                    return 1.0;
                }
                const int pct = transporter::engine::get_hw_volume_pct(hw);
                return pct >= 0 ? static_cast<double>(pct) / 100.0 : 1.0;
            })
            .withSetter([this](double v) {
                if (engine_ == nullptr) {
                    return;
                }
                const auto& hw = engine_->pipeline_snapshot().device.current_hw_string;
                if (hw.empty()) {
                    return;
                }
                const int pct = std::clamp(static_cast<int>(v * 100.0), 0, 100);
                transporter::engine::set_hw_volume_pct(hw, pct);
            }),
        sdbus::registerProperty("Position").withGetter([this] {
            if (engine_ == nullptr) {
                return std::int64_t{0};
            }
            const auto snap = engine_->pipeline_snapshot();
            const std::uint32_t rate = snap.output.hw_params_set.sample_rate_hz;
            if (rate == 0) {
                return std::int64_t{0};
            }
            const auto frames = snap.output.frames_written;
            const auto us = static_cast<std::int64_t>(
                (frames * 1'000'000ULL) / rate);
            return us;
        }),
        sdbus::registerProperty("MinimumRate").withGetter([] { return 1.0; }),
        sdbus::registerProperty("MaximumRate").withGetter([] { return 1.0; }),
        sdbus::registerProperty("CanGoNext").withGetter([this] {
            return static_cast<bool>(next_hook_);
        }),
        sdbus::registerProperty("CanGoPrevious").withGetter([this] {
            return static_cast<bool>(prev_hook_);
        }),
        sdbus::registerProperty("CanPlay").withGetter(
            [this] { return engine_ != nullptr; }),
        sdbus::registerProperty("CanPause").withGetter(
            [this] { return engine_ != nullptr; }),
        sdbus::registerProperty("CanSeek").withGetter([this] {
            if (engine_ == nullptr) {
                return false;
            }
            const auto snap = engine_->pipeline_snapshot();
            return snap.source.sample_rate_hz > 0 && snap.source.total_frames > 0;
        }),
        sdbus::registerProperty("CanControl").withGetter(
            [this] { return engine_ != nullptr; })
    ).forInterface(sdbus::InterfaceName{kIface});
}

} // namespace transporter::dbus_svc
