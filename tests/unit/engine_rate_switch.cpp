// SPDX-License-Identifier: GPL-3.0-or-later
//
// Loads a 44.1 kHz fixture, then loads a 48 kHz fixture. Asserts the engine
// emits RateSwitched and that the mock device saw a close + reopen.

#include "../../src/engine/engine_test_access.hpp"
#include "../../src/engine/output_iface.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tp = transporter::engine;
namespace td = transporter::engine::detail;

namespace {

using namespace std::chrono_literals;

class MockOutput final : public td::IOutput {
public:
    MockOutput(tp::PcmFormat fmt, std::atomic<int>* closes_)
        : fmt_(fmt), closes(closes_) {}
    const tp::PcmFormat& format() const noexcept override { return fmt_; }
    std::expected<std::size_t, tp::Error>
    write_all(std::span<const std::byte> in) override {
        // Throttle harder than engine_fsm's mock so the first track stays in
        // Playing long enough for a second load to arrive mid-flight.
        std::this_thread::sleep_for(20ms);
        const unsigned fb = fmt_.frame_bytes();
        return fb == 0 ? std::size_t{0} : in.size() / fb;
    }
    void drop_and_close() noexcept override {
        if (closes) {
            closes->fetch_add(1, std::memory_order_release);
        }
    }

private:
    tp::PcmFormat fmt_;
    std::atomic<int>* closes;
};

class MockDevice final : public td::IDevice {
public:
    std::atomic<int> opens{0};
    std::atomic<int> closes{0};

    std::expected<td::CapsView, tp::Error> probe_caps() override {
        td::CapsView v;
        v.rates = {44100, 48000};
        v.formats = {tp::SampleFormat::S16_LE};
        v.min_channels = 1;
        v.max_channels = 2;
        return v;
    }
    using td::IDevice::open;
    std::expected<std::unique_ptr<td::IOutput>, tp::Error>
    open(const tp::PcmFormat& fmt) override {
        opens.fetch_add(1, std::memory_order_release);
        return std::make_unique<MockOutput>(fmt, &closes);
    }
};

struct EventLog {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<tp::Event::Kind> kinds;
    std::vector<tp::State> states;
    tp::PcmFormat last_rate_switched_fmt{};
    bool saw_rate_switched{false};

    void on(const tp::Event& e) {
        std::lock_guard lk(mtx);
        kinds.push_back(e.kind);
        if (e.kind == tp::Event::Kind::StateChanged) {
            states.push_back(e.state);
        } else if (e.kind == tp::Event::Kind::RateSwitched) {
            last_rate_switched_fmt = e.format;
            saw_rate_switched = true;
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
};

int fail(const char* where) {
    std::fprintf(stderr, "FAIL [%s]\n", where);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <fixtures/sine_44100_16.flac> <fixtures/sine_48000_16.flac>\n",
                     argv[0]);
        return 2;
    }

    EventLog log;
    auto dev = std::make_unique<MockDevice>();
    auto* dev_ptr = dev.get();
    auto e = tp::EngineTestHooks::create_with_device(tp::EngineConfig{}, std::move(dev));
    if (!e) {
        return fail("create");
    }
    auto& engine = **e;
    engine.set_event_callback([&](const tp::Event& ev) { log.on(ev); });

    // First track at 44.1 kHz.
    if (!engine.load(argv[1])) {
        return fail("load1");
    }
    if (!log.wait_state(tp::State::Playing, 2s)) {
        return fail("wait-playing-1");
    }
    if (engine.current_format().sample_rate_hz != 44100) {
        return fail("rate-1");
    }
    if (dev_ptr->opens.load() != 1) {
        return fail("opens=1");
    }

    // Second track at 48 kHz: triggers re-open.
    if (!engine.load(argv[2])) {
        return fail("load2");
    }
    // Wait until the new track is playing, then validate.
    std::this_thread::sleep_for(200ms);
    // Allow extra slack: the engine drains+closes+reopens; state hops back to
    // Playing once the new run starts.
    bool got_48k = false;
    for (int i = 0; i < 100 && !got_48k; ++i) {
        if (engine.current_format().sample_rate_hz == 48000 &&
            engine.state() == tp::State::Playing) {
            got_48k = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    if (!got_48k) {
        return fail("transition-to-48k");
    }

    if (dev_ptr->opens.load() < 2) {
        return fail("opens<2");
    }
    if (dev_ptr->closes.load() < 1) {
        return fail("closes<1");
    }
    {
        std::lock_guard lk(log.mtx);
        if (!log.saw_rate_switched) {
            return fail("RateSwitched event");
        }
        if (log.last_rate_switched_fmt.sample_rate_hz != 48000) {
            return fail("RateSwitched fmt rate");
        }
    }

    // Stop cleanly.
    (void)engine.stop();
    (void)log.wait_state(tp::State::Idle, 2s);

    std::printf("ok engine_rate_switch (opens=%d closes=%d)\n",
                dev_ptr->opens.load(), dev_ptr->closes.load());
    return 0;
}
