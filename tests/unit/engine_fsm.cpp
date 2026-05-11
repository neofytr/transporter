// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drives the engine FSM through every transition with a mock IDevice. Asserts
// emitted events and the final state at each step.

#include "../../src/engine/engine_test_access.hpp"
#include "../../src/engine/output_iface.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace tp = transporter::engine;
namespace td = transporter::engine::detail;

namespace {

using namespace std::chrono_literals;

class MockOutput final : public td::IOutput {
public:
    MockOutput(tp::PcmFormat fmt, std::atomic<int>* writes, std::atomic<bool>* closed)
        : fmt_(fmt), writes_(writes), closed_(closed) {}
    const tp::PcmFormat& format() const noexcept override { return fmt_; }
    std::expected<std::size_t, tp::Error>
    write_all(std::span<const std::byte> in) override {
        // Simulate kernel writei throttle.
        std::this_thread::sleep_for(1ms);
        writes_->fetch_add(static_cast<int>(in.size()), std::memory_order_release);
        const unsigned fb = fmt_.frame_bytes();
        return fb == 0 ? std::size_t{0} : in.size() / fb;
    }
    void drop_and_close() noexcept override {
        if (closed_) {
            closed_->store(true, std::memory_order_release);
        }
    }

private:
    tp::PcmFormat fmt_;
    std::atomic<int>* writes_;
    std::atomic<bool>* closed_;
};

class MockDevice final : public td::IDevice {
public:
    std::atomic<int> opens{0};
    std::atomic<int> writes{0};
    std::atomic<bool> last_closed{false};

    std::expected<td::CapsView, tp::Error> probe_caps() override {
        td::CapsView v;
        v.rates = {44100, 48000, 88200, 96000, 192000};
        v.formats = {tp::SampleFormat::S16_LE, tp::SampleFormat::S24_3LE,
                     tp::SampleFormat::S32_LE};
        v.min_channels = 1;
        v.max_channels = 2;
        return v;
    }
    using td::IDevice::open;  // bring options-form into scope
    std::expected<std::unique_ptr<td::IOutput>, tp::Error>
    open(const tp::PcmFormat& fmt) override {
        opens.fetch_add(1, std::memory_order_release);
        last_closed.store(false, std::memory_order_release);
        return std::make_unique<MockOutput>(fmt, &writes, &last_closed);
    }
};

struct EventLog {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<tp::Event::Kind> kinds;
    std::vector<tp::State> states;

    void on(const tp::Event& e) {
        std::lock_guard lk(mtx);
        kinds.push_back(e.kind);
        if (e.kind == tp::Event::Kind::StateChanged) {
            states.push_back(e.state);
        }
        cv.notify_all();
    }

    bool wait_state(tp::State s, std::chrono::milliseconds timeout) {
        std::unique_lock lk(mtx);
        return cv.wait_for(lk, timeout, [&] {
            for (auto x : states) {
                if (x == s) {
                    return true;
                }
            }
            return false;
        });
    }

    bool saw(tp::Event::Kind k) const {
        for (auto x : kinds) {
            if (x == k) {
                return true;
            }
        }
        return false;
    }
};

int fail(const char* where) {
    std::fprintf(stderr, "FAIL [%s]\n", where);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <fixtures/sine_44100_16.flac>\n", argv[0]);
        return 2;
    }

    EventLog log;
    auto e = tp::EngineTestHooks::create_with_device(
        tp::EngineConfig{}, std::make_unique<MockDevice>());
    if (!e) {
        return fail("create");
    }
    auto& engine = **e;

    engine.set_event_callback([&](const tp::Event& ev) { log.on(ev); });

    if (engine.state() != tp::State::Idle) {
        return fail("initial-state");
    }

    // Idle -> Loading -> Playing
    auto lr = engine.load(argv[1]);
    if (!lr) {
        return fail("load");
    }
    if (!log.wait_state(tp::State::Playing, 2s)) {
        return fail("wait-playing");
    }
    if (!log.saw(tp::Event::Kind::TrackLoaded)) {
        return fail("track-loaded-event");
    }

    // Playing -> Paused
    if (!engine.pause()) {
        return fail("pause");
    }
    if (!log.wait_state(tp::State::Paused, 1s)) {
        return fail("wait-paused");
    }

    // Paused -> Playing
    if (!engine.play()) {
        return fail("play");
    }
    // Playing emitted twice (load + resume); state currently Playing.
    std::this_thread::sleep_for(50ms);
    if (engine.state() != tp::State::Playing) {
        return fail("resume-state");
    }

    // Playing -> Stopped -> Idle
    if (!engine.stop()) {
        return fail("stop");
    }
    if (!log.wait_state(tp::State::Idle, 2s)) {
        return fail("wait-idle-after-stop");
    }

    // Re-load + run to natural EOF: TrackEnded then Idle.
    {
        std::lock_guard lk(log.mtx);
        log.kinds.clear();
        log.states.clear();
    }
    if (!engine.load(argv[1])) {
        return fail("reload");
    }
    if (!log.wait_state(tp::State::Playing, 2s)) {
        return fail("wait-playing-2");
    }
    // Wait for natural EOF (mock writes are no-ops; decoder finishes fast).
    if (!log.wait_state(tp::State::Idle, 5s)) {
        return fail("wait-idle-eof");
    }
    if (!log.saw(tp::Event::Kind::TrackEnded)) {
        return fail("track-ended-event");
    }

    // Gapless preload: load track, immediately preload the same file
    // (same format guarantees a gapless swap at EOF). The decoder thread
    // should swap in the preloaded decoder and emit TrackLoaded, not
    // TrackEnded. State stays Playing after the swap.
    {
        std::lock_guard lk(log.mtx);
        log.kinds.clear();
        log.states.clear();
    }
    if (!engine.load(argv[1])) {
        return fail("gapless-load");
    }
    // Stage the preload immediately — before the decoder can reach EOF.
    // The mock output's 1 ms/write throttle gives ~172 ms of headroom for
    // a 2-second fixture, so this call is well within the window.
    if (!engine.preload(argv[1])) {
        return fail("gapless-preload");
    }
    // Now wait for Playing (engine may still be Loading).
    if (!log.wait_state(tp::State::Playing, 2s)) {
        return fail("gapless-wait-playing");
    }
    // Clear the log so we only see events after the initial TrackLoaded.
    {
        std::lock_guard lk(log.mtx);
        log.kinds.clear();
        log.states.clear();
    }
    // Wait for the gapless swap to fire a second TrackLoaded. Give ample
    // time: 2 s fixture × mock drain rate.
    {
        std::unique_lock lk(log.mtx);
        const bool got = log.cv.wait_for(lk, 6s, [&] {
            for (auto k : log.kinds) {
                if (k == tp::Event::Kind::TrackLoaded) {
                    return true;
                }
            }
            return false;
        });
        if (!got) {
            return fail("gapless-track-loaded-event");
        }
        for (auto k : log.kinds) {
            if (k == tp::Event::Kind::TrackEnded) {
                return fail("gapless-spurious-track-ended");
            }
        }
    }
    // Engine should still be playing (not stopped/idle) after the swap.
    std::this_thread::sleep_for(20ms);
    if (engine.state() == tp::State::Idle || engine.state() == tp::State::Stopped) {
        return fail("gapless-state-not-playing");
    }

    std::printf("ok engine_fsm\n");
    return 0;
}
