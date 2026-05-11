// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_SRC_DBUS_MPRIS_PLAYER_HPP
#define TRANSPORTER_SRC_DBUS_MPRIS_PLAYER_HPP

#include <transporter/engine/engine.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace transporter::dbus_svc {

// Adaptor for org.mpris.MediaPlayer2.Player.
//
// State the adaptor caches:
//   - The currently loaded track's MPRIS metadata, computed from the engine's
//     PipelineSnapshot at load-time and after each rate switch. Properties
//     pull from the cache; the audio thread is never touched.
//   - The last known engine state. Updated by the polling loop in
//     DbusService; the adaptor itself does not poll.
//   - A trackid counter. MPRIS requires an opaque object-path-shaped id per
//     load. We use /org/mpris/MediaPlayer2/transporter/track/<n>.
class MprisPlayer {
public:
    using NextHook = std::function<std::optional<std::filesystem::path>()>;
    using PreviousHook = std::function<std::optional<std::filesystem::path>()>;
    using LoadHook = std::function<void(const std::filesystem::path&)>;
    using ShuffleGetter = std::function<bool()>;
    using ShuffleSetter = std::function<void(bool)>;
    using LoopStatusGetter = std::function<std::string()>;
    using LoopStatusSetter = std::function<void(const std::string&)>;

    MprisPlayer(sdbus::IObject& obj,
                transporter::engine::Engine* engine,
                NextHook next,
                PreviousHook prev,
                LoadHook load,
                ShuffleGetter shuffle_get = {},
                ShuffleSetter shuffle_set = {},
                LoopStatusGetter loop_get = {},
                LoopStatusSetter loop_set = {});

    MprisPlayer(const MprisPlayer&) = delete;
    MprisPlayer& operator=(const MprisPlayer&) = delete;

    // Recompute and cache the MPRIS metadata variant map from the engine's
    // current pipeline snapshot. Emits PropertiesChanged for Metadata + the
    // playback-status-coupled props on transition. Safe to call from the
    // event-loop thread or from the polling thread.
    void refresh_metadata();

    // Update PlaybackStatus mirror + emit PropertiesChanged. Called by the
    // service whenever a state event arrives or the polling loop notices a
    // transition.
    void notify_state(transporter::engine::State s);

    // Emit PropertiesChanged for Shuffle / LoopStatus so connected clients
    // (status bars, playerctl watchers) pick up GUI-driven changes promptly.
    void notify_shuffle();
    void notify_loop_status();
    void notify_volume();

private:
    void register_vtable();
    std::map<std::string, sdbus::Variant>
    build_metadata_locked() const;

    static std::string playback_status_for(transporter::engine::State s);

    sdbus::IObject& obj_;
    transporter::engine::Engine* engine_;

    NextHook next_hook_;
    PreviousHook prev_hook_;
    LoadHook load_hook_;
    ShuffleGetter shuffle_get_;
    ShuffleSetter shuffle_set_;
    LoopStatusGetter loop_get_;
    LoopStatusSetter loop_set_;

    mutable std::mutex meta_mtx_;
    std::map<std::string, sdbus::Variant> metadata_;  // current track
    std::uint64_t track_counter_ = 0;
    std::string last_status_;
};

} // namespace transporter::dbus_svc

#endif
