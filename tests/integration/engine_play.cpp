// SPDX-License-Identifier: GPL-3.0-or-later
//
// Engine integration driver. Constructs a real engine over the given hw:
// device, loads the file, plays to natural EOF, exits.

#include <transporter/engine/engine.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>

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

void print_usage(const char* argv0) {
    std::fprintf(stderr, "usage: %s <hw:CARD=X,DEV=Y> <file>\n", argv0);
}

const char* state_name(tp::State s) {
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
        if (ev.kind == tp::Event::Kind::StateChanged) {
            std::fprintf(stderr, "state -> %s\n", state_name(ev.state));
        } else if (ev.kind == tp::Event::Kind::TrackLoaded) {
            std::fprintf(stderr, "loaded: rate=%u ch=%u total=%llu\n",
                         ev.format.sample_rate_hz, ev.format.channels,
                         static_cast<unsigned long long>(ev.total_frames));
        } else if (ev.kind == tp::Event::Kind::TrackEnded) {
            std::fprintf(stderr, "track ended\n");
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

    {
        std::unique_lock lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(60), [&] { return ended || errored; });
    }
    return errored ? 4 : 0;
}
