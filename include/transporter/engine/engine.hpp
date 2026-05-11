// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_ENGINE_ENGINE_HPP
#define TRANSPORTER_ENGINE_ENGINE_HPP

#include <transporter/engine/decoder.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/engine/ring.hpp>
#include <transporter/engine/telemetry.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <ostream>
#include <string>

namespace transporter::hotplug {
class IMonitor;
} // namespace transporter::hotplug

namespace transporter::engine {

// Lifecycle states. The engine never re-enters Loading from Idle without an
// explicit load() call. Stopped is a transient sink that returns to Idle once
// teardown completes; consumers may observe it via StateChanged.
//
// Disconnected is functionally Paused (audio thread stopped, output closed)
// but reported separately so the GUI can render a "DAC disconnected" badge.
// The engine remembers whether the user had explicitly paused before the
// disconnect: on same-DAC return, that intent is preserved (Paused vs.
// Playing).
enum class State : std::uint8_t {
    Idle,
    Loading,
    Playing,
    Paused,
    Stopped,
    Error,
    Disconnected,
};

// Factory for the hotplug monitor. Default (empty factory) constructs the
// real udev monitor. Tests inject a mock. Returning a null pointer disables
// hotplug entirely (engine never enters Disconnected). move-only because
// callers commonly capture a unique_ptr<IMonitor> they prepared up front.
using HotplugFactory =
    std::move_only_function<std::unique_ptr<transporter::hotplug::IMonitor>()>;

// Construction-time configuration. ring_capacity_bytes is rounded up to the
// next power of two (SpscByteRing requirement). target_latency is advisory
// for Phase 3; period sizing in alsa::Output already targets ~12 ms periods.
struct EngineConfig {
    std::string device_id;
    std::size_t ring_capacity_bytes = 1u << 20;
    std::chrono::milliseconds target_latency{50};
    // Hotplug monitor factory. Empty = real libudev monitor (Engine::create
    // resolves to open_udev_monitor()). Tests inject a mock; tests that
    // don't care about hotplug pass a factory that returns nullptr.
    HotplugFactory hotplug_factory;
};

// Asynchronous notification from the engine. Delivered on the engine's worker
// thread (never the audio thread, never the API thread). The callback must
// return promptly: a long-running callback blocks subsequent events.
struct Event {
    enum class Kind : std::uint8_t {
        StateChanged,
        TrackLoaded,
        TrackEnded,
        RateSwitched,
        ErrorOccurred,
        DeviceLost,    // active DAC disconnected; engine -> Disconnected
        DeviceReturn,  // active DAC reconnected; engine resumes prior intent
    };
    Kind kind{Kind::StateChanged};
    State state{State::Idle};       // StateChanged
    PcmFormat format{};             // TrackLoaded, RateSwitched
    Tags tags{};                    // TrackLoaded
    std::uint64_t total_frames{0};  // TrackLoaded
    Error error{ErrorCode::DeviceOpenFailed, ""};  // ErrorOccurred
    std::filesystem::path file_path{};  // TrackLoaded: path of the loaded file
};

using EventCallback = std::function<void(const Event&)>;

// Engine. Owns the active decoder, ALSA device, and the decoder + audio
// threads. Public API methods are non-blocking command posts that return
// std::expected for synchronous validation only; the actual state transition
// completes asynchronously and surfaces via Event.
//
// Threading: load/play/pause/stop may be called from any thread. set_event_-
// callback must be called before subscribing.
//
// Lifetime: the destructor stops the audio thread, closes the device, and
// joins the decoder thread before returning. It is safe to destroy the
// engine from any state.
class Engine {
public:
    static std::expected<std::unique_ptr<Engine>, Error> create(EngineConfig cfg);

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;
    ~Engine();

    // Validate-and-post. Returns synchronously after enqueueing the command.
    // Decoder open and capability match happen on the engine worker thread;
    // failures there surface via ErrorOccurred. Synchronous failure returns
    // here only for argument-level problems (missing file, decoder factory
    // refused).
    std::expected<void, Error> load(std::filesystem::path file);
    std::expected<void, Error> play();
    std::expected<void, Error> pause();
    std::expected<void, Error> stop();
    // Seek to a PCM frame offset within the current track. Non-blocking;
    // actual seek completes on the engine worker thread (~12 ms latency).
    std::expected<void, Error> seek(std::uint64_t frame);

    // Open the next decoder in the background without disturbing the active
    // run. On current-track EOF, if the preloaded format exactly matches the
    // live format, the decoder thread swaps in next_decoder seamlessly and
    // continues filling the ring without stopping. Format mismatch or missing
    // preload falls through to the normal TrackEnded path.
    std::expected<void, Error> preload(std::filesystem::path file);

    // Cancel any pending preload (called when the user explicitly loads a
    // different track before the current one ends).
    void cancel_preload();

    State state() const noexcept;
    PcmFormat current_format() const noexcept;

    // Snapshot the full pipeline for the GUI / telemetry dump. Safe to call
    // from any non-RT thread at >= 10 Hz. Holds engine-side mutexes only
    // briefly (decoder pointer + current hw string); the audio path is
    // unaffected. Returns by value; never throws on the happy path.
    PipelineSnapshot pipeline_snapshot() const;

    // Dump the in-memory trace ring as one event per line. Format is stable
    // enough for grep / awk; not a binary protocol.
    void dump_trace(std::ostream& os) const;

    // Returns pointer to the spectrum sample ring. Carries float32 mono
    // (channel-0 only) samples at the native playback rate. GUI thread owns
    // the consumer side; audio thread is the producer. Null when no track
    // is loaded.
    SpscByteRing* spectrum_ring() noexcept;

    // Single-listener. Caller-side fan-out is the consumer's job.
    void set_event_callback(EventCallback cb);

private:
    Engine();
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Test-only seam (src/engine/engine_test_access.hpp). Friended to allow
    // the unit tests to inject a mock device without IDevice on the public
    // API.
    friend struct EngineTestHooks;
};

struct EngineTestHooks; // declared above; defined in engine_test_access.hpp

} // namespace transporter::engine

#endif
