// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_DBUS_SERVICE_HPP
#define TRANSPORTER_DBUS_SERVICE_HPP

#include <transporter/engine/engine.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/library/library.hpp>

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace transporter::dbus_svc {

// Toggle in [dbus] section of config.toml. When false, start() returns nullptr
// and the caller skips DBus publication entirely. When true, start() attempts
// to acquire the well-known bus name; if it is already taken (a second
// instance) the caller decides whether to fail or warn-and-continue based on
// the returned Error.
struct Config {
    bool enabled = true;
};

// Hooks the controlling object exposes to the service so MPRIS Next/Previous
// can drive the in-process queue. The service does not own the queue; the
// caller (gui::run / gui::run_headless) does. Hooks are invoked on the DBus
// event-loop thread; implementations must post work to whatever thread owns
// the engine and return promptly.
//
// `next` / `previous` advance the queue and return the new file path (or empty
// optional when there is nothing to move to). `reload_config` re-reads the
// caller's config file in-place, returning true if any hot-reloadable field
// was applied. `rescan_library` kicks the scanner thread.
struct Hooks {
    std::function<std::optional<std::filesystem::path>()> next;
    std::function<std::optional<std::filesystem::path>()> previous;
    std::function<bool()> reload_config;
    std::function<void()> rescan_library;
    std::function<bool()> shuffle_getter;
    std::function<void(bool)> shuffle_setter;
    std::function<std::string()> loop_status_getter;
    std::function<void(const std::string&)> loop_status_setter;
};

// Owns the session-bus connection, the event-loop thread, and all adaptors
// for the two services published at /org/mpris/MediaPlayer2 and
// /org/mpris/MediaPlayer2/transporter. Engine and library pointers are
// borrowed; the caller must keep them alive for the lifetime of the service.
//
// Threading: start() runs on the caller's thread and synchronously connects
// + claims the bus name + spawns the event-loop thread. All adaptor callbacks
// fire on the event-loop thread. Engine command posts (load/play/pause/stop)
// are non-blocking, so the audio thread is never stalled by DBus traffic.
class DbusService {
public:
    static std::expected<std::unique_ptr<DbusService>, transporter::engine::Error>
    start(transporter::engine::Engine* engine,
          transporter::library::Library* library,
          Hooks hooks = {},
          Config cfg = {});

    DbusService(const DbusService&) = delete;
    DbusService& operator=(const DbusService&) = delete;
    DbusService(DbusService&&) = delete;
    DbusService& operator=(DbusService&&) = delete;
    ~DbusService();

    // Inform the service about a fresh load. The MPRIS metadata + the custom
    // BitPerfect signal are recomputed on the next poll cycle; calling this
    // immediately on load is an optimization, not a correctness requirement.
    void notify_track_loaded();

    // Emit MPRIS PropertiesChanged for Shuffle / LoopStatus. Call from the
    // GUI thread whenever the user toggles these via the UI controls.
    void notify_shuffle_changed();
    void notify_loop_status_changed();

    // Invoke from the controlling object when an externally-driven Quit
    // happens (Ctrl-C, GUI close, etc.) so the service stops emitting before
    // the engine goes away.
    void shutdown();

private:
    DbusService();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace transporter::dbus_svc

#endif
