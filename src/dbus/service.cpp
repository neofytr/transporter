// SPDX-License-Identifier: GPL-3.0-or-later
//
// DbusService: top-level glue. Brings up the bus connection, registers all
// adaptors at the right object paths, spawns the event-loop thread, and runs
// a 4 Hz polling loop that watches engine state for the bit-perfect transition
// signal.

#include <transporter/dbus/service.hpp>

#include "bus.hpp"
#include "mpris_metadata.hpp"
#include "mpris_player.hpp"
#include "mpris_root.hpp"
#include "mpris_tracklist.hpp"
#include "snapshot_serialize.hpp"
#include "transporter_iface.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/telemetry.hpp>
#include <transporter/library/library.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace transporter::dbus_svc {

namespace {

constexpr const char* kBusName = "org.mpris.MediaPlayer2.transporter";
constexpr const char* kMprisPath = "/org/mpris/MediaPlayer2";
constexpr const char* kCustomPath = "/org/mpris/MediaPlayer2/transporter";

using transporter::engine::Error;
using transporter::engine::ErrorCode;

} // namespace

struct DbusService::Impl {
    std::unique_ptr<Bus> bus;
    std::unique_ptr<sdbus::IObject> mpris_obj;
    std::unique_ptr<sdbus::IObject> custom_obj;

    std::unique_ptr<MprisRoot> root;
    std::unique_ptr<MprisPlayer> player;
    std::unique_ptr<MprisTrackList> tracklist;
    std::unique_ptr<TransporterIface> custom;

    transporter::engine::Engine* engine = nullptr;
    transporter::library::Library* library = nullptr;

    std::atomic<bool> stop_flag{false};
    std::thread poller;
    std::mutex poll_mtx;
    std::condition_variable poll_cv;

    std::string last_bp_level;
    transporter::engine::State last_state = transporter::engine::State::Idle;

    void poll_loop();
};

DbusService::DbusService() : impl_(std::make_unique<Impl>()) {}
DbusService::~DbusService() {
    shutdown();
}

void DbusService::shutdown() {
    if (!impl_) {
        return;
    }
    if (!impl_->stop_flag.exchange(true)) {
        impl_->poll_cv.notify_all();
        if (impl_->poller.joinable()) {
            impl_->poller.join();
        }
    }
    if (impl_->bus) {
        impl_->bus->stop_event_loop();
    }
    impl_->custom.reset();
    impl_->tracklist.reset();
    impl_->player.reset();
    impl_->root.reset();
    impl_->custom_obj.reset();
    impl_->mpris_obj.reset();
    impl_->bus.reset();
}

void DbusService::notify_track_loaded() {
    if (impl_ && impl_->player) {
        impl_->player->refresh_metadata();
    }
}

void DbusService::notify_shuffle_changed() {
    if (impl_ && impl_->player) {
        impl_->player->notify_shuffle();
    }
}

void DbusService::notify_loop_status_changed() {
    if (impl_ && impl_->player) {
        impl_->player->notify_loop_status();
    }
}

void DbusService::notify_volume_changed() {
    if (impl_ && impl_->player) {
        impl_->player->notify_volume();
    }
}

void DbusService::Impl::poll_loop() {
    using namespace std::chrono_literals;
    while (!stop_flag.load(std::memory_order_acquire)) {
        if (engine != nullptr) {
            const auto snap = engine->pipeline_snapshot();

            const std::string level{
                bit_perfect_level_name(snap.bit_perfect.level)};
            if (level != last_bp_level) {
                last_bp_level = level;
                if (custom) {
                    custom->emit_bitperfect_changed(
                        level, snap.bit_perfect.qualifications);
                }
            }

            const auto state = snap.engine_state;
            if (state != last_state) {
                last_state = state;
                if (player) {
                    player->notify_state(state);
                }
            }
        }

        std::unique_lock lk(poll_mtx);
        poll_cv.wait_for(lk, 250ms, [this] {
            return stop_flag.load(std::memory_order_acquire);
        });
    }
}

std::expected<std::unique_ptr<DbusService>, Error>
DbusService::start(transporter::engine::Engine* engine,
                   transporter::library::Library* library,
                   Hooks hooks,
                   Config cfg) {
    if (!cfg.enabled) {
        return std::unexpected(
            Error{ErrorCode::InvalidArgument, "DBus disabled by config"});
    }

    auto svc = std::unique_ptr<DbusService>(new DbusService());
    auto& impl = *svc->impl_;
    impl.engine = engine;
    impl.library = library;

    auto bus_or = Bus::open(kBusName);
    if (!bus_or) {
        return std::unexpected(bus_or.error());
    }
    impl.bus = std::move(*bus_or);

    auto& conn = impl.bus->connection();

    // Two object paths share one connection. sdbus-c++ allows multiple
    // interfaces to be registered on a single IObject; the standard MPRIS
    // path holds Root + Player + TrackList, the custom path holds the
    // transporter interface.
    try {
        impl.mpris_obj = sdbus::createObject(conn, sdbus::ObjectPath{kMprisPath});
        impl.custom_obj = sdbus::createObject(conn, sdbus::ObjectPath{kCustomPath});
    } catch (const sdbus::Error& e) {
        return std::unexpected(Error{
            ErrorCode::InvalidArgument,
            std::string{"DBus object creation failed: "} + e.what()});
    }

    auto load_hook = [engine](const std::filesystem::path& p) {
        if (engine == nullptr) {
            return;
        }
        if (auto r = engine->load(p); r) {
            (void)engine->play();
        }
    };

    auto reload_hook = std::move(hooks.reload_config);
    auto rescan_hook = std::move(hooks.rescan_library);

    try {
        impl.root = std::make_unique<MprisRoot>(*impl.mpris_obj, [&impl] {
            // Quit: leave the event loop. Process exit is the caller's job.
            impl.stop_flag.store(true);
            impl.poll_cv.notify_all();
            if (impl.bus) {
                impl.bus->stop_event_loop();
            }
        });
        impl.player = std::make_unique<MprisPlayer>(
            *impl.mpris_obj, engine,
            std::move(hooks.next), std::move(hooks.previous), load_hook,
            std::move(hooks.shuffle_getter), std::move(hooks.shuffle_setter),
            std::move(hooks.loop_status_getter), std::move(hooks.loop_status_setter));

        // TrackList: minimal queue exposure. The current track id is the
        // counter the Player adaptor maintains; we surface a single-entry
        // list when something is loaded, otherwise empty.
        auto ids_provider = [engine]() -> std::vector<sdbus::ObjectPath> {
            if (engine == nullptr) {
                return {};
            }
            const auto snap = engine->pipeline_snapshot();
            if (snap.source.file_path.empty()) {
                return {};
            }
            return {sdbus::ObjectPath{
                "/org/mpris/MediaPlayer2/transporter/track/current"}};
        };
        auto meta_provider = [engine](const sdbus::ObjectPath& id)
            -> std::map<std::string, sdbus::Variant> {
            if (engine == nullptr) {
                std::map<std::string, sdbus::Variant> m;
                m.emplace("mpris:trackid", sdbus::Variant{id});
                return m;
            }
            return mpris_metadata_from_snapshot(
                engine->pipeline_snapshot(), std::string{id});
        };
        impl.tracklist = std::make_unique<MprisTrackList>(
            *impl.mpris_obj, std::move(ids_provider), std::move(meta_provider));

        impl.custom = std::make_unique<TransporterIface>(
            *impl.custom_obj, engine, library,
            std::move(reload_hook), std::move(rescan_hook));
    } catch (const sdbus::Error& e) {
        return std::unexpected(Error{
            ErrorCode::InvalidArgument,
            std::string{"DBus vtable registration failed: "} + e.what()});
    }

    // Engine event subscription: bridges async events to MPRIS signals so
    // PlaybackStatus / Metadata flip without waiting for the polling cycle.
    if (engine != nullptr) {
        // Note: there is a single event callback slot on the engine; the
        // GUI/headless caller already installs one. We do not steal it.
        // The 4 Hz poll covers our needs.
    }

    impl.bus->start_event_loop();

    impl.poller = std::thread([p = svc->impl_.get()] { p->poll_loop(); });

    return svc;
}

} // namespace transporter::dbus_svc
