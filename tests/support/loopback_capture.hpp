// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_TESTS_SUPPORT_LOOPBACK_CAPTURE_HPP
#define TRANSPORTER_TESTS_SUPPORT_LOOPBACK_CAPTURE_HPP

#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace transporter::testing {

// Configuration for the capture thread. `hw_name` is an ALSA device string
// pointing at a `Loopback`-card capture endpoint (e.g. `hw:CARD=Loopback,DEV=1`).
// `silence_window_ms` controls when capture stops: once the most recent
// window contains no non-silent samples and an explicit stop has been
// requested, the thread exits.
struct LoopbackCaptureConfig {
    std::string hw_name;
    transporter::engine::PcmFormat format;
    std::chrono::milliseconds silence_window_ms{50};
    std::size_t period_frames = 1024;
};

// Background snd_pcm capture thread. Reads interleaved PCM frames into an
// internal buffer; stop() flips a flag and join() returns the captured bytes.
// All ALSA work happens on the spawned thread; the public API is callable
// from any other thread.
class LoopbackCapture {
public:
    LoopbackCapture();
    ~LoopbackCapture();
    LoopbackCapture(const LoopbackCapture&) = delete;
    LoopbackCapture& operator=(const LoopbackCapture&) = delete;
    LoopbackCapture(LoopbackCapture&&) noexcept;
    LoopbackCapture& operator=(LoopbackCapture&&) noexcept;

    // Open the capture device and spawn the reader thread. Returns on the
    // first successful prepare; capture begins immediately.
    std::expected<void, transporter::engine::Error>
    start(const LoopbackCaptureConfig& cfg);

    // Signal the reader to drain the trailing silence window and exit. Safe
    // to call from any thread. Idempotent.
    void stop() noexcept;

    // Block until the reader has exited. Returns the accumulated capture
    // bytes (interleaved PCM in `cfg.format`). Calling this without a prior
    // start() returns an empty buffer.
    std::vector<std::byte> join();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace transporter::testing

#endif
