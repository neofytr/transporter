// SPDX-License-Identifier: GPL-3.0-or-later
//
// Loads a 24-bit fixture against a mock device that does not advertise
// S24_3LE. Asserts the engine emits ErrorOccurred with FormatNotSupported
// and stays in Idle.

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
#include <thread>
#include <utility>
#include <vector>

namespace tp = transporter::engine;
namespace td = transporter::engine::detail;

namespace {

using namespace std::chrono_literals;

class MockOutput final : public td::IOutput {
public:
    explicit MockOutput(tp::PcmFormat fmt) : fmt_(fmt) {}
    const tp::PcmFormat& format() const noexcept override { return fmt_; }
    std::expected<void, tp::Error>
    write_all(std::span<const std::byte> /*in*/) override { return {}; }
    void drop_and_close() noexcept override {}

private:
    tp::PcmFormat fmt_;
};

class MockDevice final : public td::IDevice {
public:
    std::atomic<int> opens{0};

    std::expected<td::CapsView, tp::Error> probe_caps() override {
        td::CapsView v;
        v.rates = {44100, 48000};
        // Deliberately omit S24_3LE.
        v.formats = {tp::SampleFormat::S16_LE, tp::SampleFormat::S32_LE};
        v.min_channels = 1;
        v.max_channels = 2;
        return v;
    }
    std::expected<std::unique_ptr<td::IOutput>, tp::Error>
    open(const tp::PcmFormat& fmt) override {
        opens.fetch_add(1, std::memory_order_release);
        return std::make_unique<MockOutput>(fmt);
    }
};

struct EventLog {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<tp::Event::Kind> kinds;
    tp::Error last_error{tp::ErrorCode::WriteFailed, ""};
    bool saw_error{false};

    void on(const tp::Event& e) {
        std::lock_guard lk(mtx);
        kinds.push_back(e.kind);
        if (e.kind == tp::Event::Kind::ErrorOccurred) {
            last_error = e.error;
            saw_error = true;
        }
        cv.notify_all();
    }

    bool wait_error(std::chrono::milliseconds timeout) {
        std::unique_lock lk(mtx);
        return cv.wait_for(lk, timeout, [&] { return saw_error; });
    }
};

int fail(const char* where) {
    std::fprintf(stderr, "FAIL [%s]\n", where);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <fixtures/sine_44100_24.wav>\n", argv[0]);
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

    if (!engine.load(argv[1])) {
        return fail("load (post)");
    }
    if (!log.wait_error(2s)) {
        return fail("wait-error");
    }

    {
        std::lock_guard lk(log.mtx);
        if (log.last_error.code != tp::ErrorCode::FormatNotSupported) {
            return fail("error code");
        }
        if (log.last_error.rejection != tp::FormatRejection::SampleFormatNotSupported) {
            return fail("rejection sub-reason");
        }
    }

    // No device open should have happened.
    if (dev_ptr->opens.load() != 0) {
        return fail("device opened despite refusal");
    }
    // Engine should be Idle (no prior run).
    std::this_thread::sleep_for(50ms);
    if (engine.state() != tp::State::Idle) {
        return fail("not Idle after refusal");
    }

    std::printf("ok engine_format_refused\n");
    return 0;
}
