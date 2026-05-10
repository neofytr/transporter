// SPDX-License-Identifier: GPL-3.0-or-later
//
// Telemetry dump driver. Constructs a real engine over a hw: device, loads
// a file, prints a one-line snapshot every 250 ms while playing, exits at
// EOF (or error).

#include <transporter/engine/engine.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/engine/telemetry.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace tp = transporter::engine;

namespace {

const char* state_name(tp::State s) noexcept {
    switch (s) {
    case tp::State::Idle:    return "Idle";
    case tp::State::Loading: return "Loading";
    case tp::State::Playing: return "Playing";
    case tp::State::Paused:  return "Paused";
    case tp::State::Stopped: return "Stopped";
    case tp::State::Error:   return "Error";
    }
    return "?";
}

const char* verdict_name(tp::BitPerfectVerdict::Level v) noexcept {
    switch (v) {
    case tp::BitPerfectVerdict::Level::Yes:       return "YES";
    case tp::BitPerfectVerdict::Level::Qualified: return "QUALIFIED";
    case tp::BitPerfectVerdict::Level::No:        return "NO";
    }
    return "?";
}

void print_usage(const char* argv0) {
    std::fprintf(stderr, "usage: %s <hw:CARD=X,DEV=Y> <file>\n", argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    tp::EngineConfig cfg;
    cfg.device_id = argv[1];

    auto e = tp::Engine::create(std::move(cfg));
    if (!e) {
        std::fprintf(stderr, "create [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(e.error().code).size()),
                     tp::error_code_name(e.error().code).data(),
                     e.error().message.c_str());
        return 2;
    }

    std::mutex mtx;
    std::condition_variable cv;
    bool ended = false;
    bool errored = false;
    e.value()->set_event_callback([&](const tp::Event& ev) {
        if (ev.kind == tp::Event::Kind::TrackEnded) {
            std::lock_guard lk(mtx);
            ended = true;
            cv.notify_all();
        } else if (ev.kind == tp::Event::Kind::ErrorOccurred) {
            std::fprintf(stderr, "error [%.*s]: %s\n",
                         static_cast<int>(tp::error_code_name(ev.error.code).size()),
                         tp::error_code_name(ev.error.code).data(),
                         ev.error.message.c_str());
            std::lock_guard lk(mtx);
            errored = true;
            cv.notify_all();
        }
    });

    auto lr = e.value()->load(argv[2]);
    if (!lr) {
        std::fprintf(stderr, "load [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(lr.error().code).size()),
                     tp::error_code_name(lr.error().code).data(),
                     lr.error().message.c_str());
        return 3;
    }

    using namespace std::chrono_literals;
    while (true) {
        {
            std::unique_lock lk(mtx);
            if (cv.wait_for(lk, 250ms, [&] { return ended || errored; })) {
                break;
            }
        }
        const tp::PipelineSnapshot s = e.value()->pipeline_snapshot();
        std::fprintf(stdout,
                     "state=%s frames_wr=%llu xrun=%u ring_fill_us=%lld "
                     "rt=%s verdict=%s\n",
                     state_name(s.engine_state),
                     static_cast<unsigned long long>(s.output.frames_written),
                     s.output.xrun_count,
                     static_cast<long long>(s.ring.fill_us.count()),
                     s.realtime.status.mode == tp::rt::Mode::Fifo ? "FIFO" : "OTHER",
                     verdict_name(s.bit_perfect.level));
        std::fflush(stdout);
    }
    return errored ? 4 : 0;
}
