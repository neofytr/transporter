// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drives the engine over a mock IDevice with sine_44100_16.flac, sleeps
// 50 ms into Playing, captures Engine::pipeline_snapshot(), and asserts:
//   - frames_produced > 0 and frames_written > 0
//   - format_match.matched_ok with rate 44100
//   - bit_perfect.level is Yes or Qualified (depending on RT availability)

#include "../../src/engine/engine_test_access.hpp"
#include "../../src/engine/output_iface.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/engine/telemetry.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

namespace tp = transporter::engine;
namespace td = transporter::engine::detail;

namespace {

using namespace std::chrono_literals;

class MockOutput final : public td::IOutput {
public:
    explicit MockOutput(tp::PcmFormat fmt) : fmt_(fmt) {}
    const tp::PcmFormat& format() const noexcept override { return fmt_; }
    std::expected<std::size_t, tp::Error>
    write_all(std::span<const std::byte> in) override {
        // Throttle hard so the snapshot has time to observe a non-trivial
        // playing window. ~5 ms per call.
        std::this_thread::sleep_for(5ms);
        const unsigned fb = fmt_.frame_bytes();
        return fb == 0 ? std::size_t{0} : in.size() / fb;
    }
    void drop_and_close() noexcept override {}
    PeriodInfo period_info() const noexcept override {
        PeriodInfo p{};
        p.period_frames = 528;     // ~12 ms at 44.1 k
        p.periods = 4;
        p.buffer_frames = 528 * 4;
        return p;
    }
private:
    tp::PcmFormat fmt_;
};

class MockDevice final : public td::IDevice {
public:
    std::expected<td::CapsView, tp::Error> probe_caps() override {
        td::CapsView v;
        v.rates = {44100, 48000, 88200, 96000};
        v.formats = {tp::SampleFormat::S16_LE, tp::SampleFormat::S24_3LE,
                     tp::SampleFormat::S32_LE};
        v.min_channels = 1;
        v.max_channels = 2;
        return v;
    }
    using td::IDevice::open;
    std::expected<std::unique_ptr<td::IOutput>, tp::Error>
    open(const tp::PcmFormat& fmt) override {
        return std::make_unique<MockOutput>(fmt);
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

    auto e = tp::EngineTestHooks::create_with_device(
        tp::EngineConfig{}, std::make_unique<MockDevice>());
    if (!e) {
        return fail("create");
    }
    auto& engine = **e;

    // Wire a small wait-for-Playing.
    std::mutex mtx;
    std::condition_variable cv;
    bool playing = false;
    engine.set_event_callback([&](const tp::Event& ev) {
        if (ev.kind == tp::Event::Kind::StateChanged &&
            ev.state == tp::State::Playing) {
            std::lock_guard lk(mtx);
            playing = true;
            cv.notify_all();
        }
    });

    auto lr = engine.load(argv[1]);
    if (!lr) {
        return fail("load");
    }
    {
        std::unique_lock lk(mtx);
        if (!cv.wait_for(lk, 2s, [&] { return playing; })) {
            return fail("wait playing");
        }
    }
    std::this_thread::sleep_for(80ms);

    const tp::PipelineSnapshot s = engine.pipeline_snapshot();

    if (s.engine_state != tp::State::Playing &&
        s.engine_state != tp::State::Stopped &&
        s.engine_state != tp::State::Idle) {
        return fail("engine_state");
    }
    if (!s.format_match.matched_ok) {
        return fail("format_match.matched_ok");
    }
    if (s.format_match.matched.sample_rate_hz != 44100) {
        return fail("matched rate");
    }
    if (s.decoder.frames_produced == 0) {
        return fail("frames_produced");
    }
    if (s.output.frames_written == 0) {
        return fail("frames_written");
    }
    if (s.source.sample_rate_hz != 44100) {
        return fail("source rate");
    }
    if (s.source.channels == 0) {
        return fail("source channels");
    }
    if (s.output.period_size_frames == 0) {
        return fail("period frames");
    }

    // RT environment dependent: in this CI box we expect Other; on a tuned
    // setup we'd expect Fifo. Either is a valid Phase 5 outcome.
    using L = tp::BitPerfectVerdict::Level;
    if (s.bit_perfect.level == L::No) {
        return fail("bit_perfect should be Yes or Qualified given matched format");
    }
    if (!s.bit_perfect.no_resampling_in_flight) {
        return fail("bit_perfect.no_resampling_in_flight");
    }
    if (!s.bit_perfect.digital_path_bitperfect) {
        return fail("bit_perfect.digital_path_bitperfect");
    }

    std::printf("ok pipeline_snapshot frames_dec=%llu frames_wr=%llu "
                "ring_fill_b=%zu ring_fill_us=%lld xrun=%u rt=%s "
                "verdict=%s\n",
                static_cast<unsigned long long>(s.decoder.frames_produced),
                static_cast<unsigned long long>(s.output.frames_written),
                s.ring.fill_bytes,
                static_cast<long long>(s.ring.fill_us.count()),
                s.output.xrun_count,
                s.realtime.status.mode == tp::rt::Mode::Fifo ? "Fifo" : "Other",
                s.bit_perfect.level == L::Yes ? "Yes" : "Qualified");
    return 0;
}
