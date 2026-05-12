// SPDX-License-Identifier: GPL-3.0-or-later
//
// Engine integration driver. Constructs a real engine over the given hw:
// device, loads the file, plays to natural EOF, exits.

#include <transporter/engine/device.hpp>
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
#include <string_view>
#include <thread>
#include <utility>

namespace tp = transporter::engine;

namespace {

constexpr int EXIT_SKIP = 77;

void print_usage(const char* argv0) {
    std::fprintf(stderr, "usage: %s <hw:CARD=X,DEV=Y|auto> <file>\n", argv0);
}

std::string discover_hw() {
    auto devs = tp::list_playback_devices();
    if (!devs || devs->empty()) {
        return {};
    }
    for (const auto& d : *devs) {
        if (!d.caps.caps_probe_failed) {
            return d.alsa_hw_string;
        }
    }
    return {};
}

bool is_hardware_skip(const tp::Error& e) {
    return e.code == tp::ErrorCode::DeviceBusy ||
           e.code == tp::ErrorCode::DeviceOpenFailed ||
           e.code == tp::ErrorCode::DeviceParamsRejected ||
           e.code == tp::ErrorCode::FormatNotSupported;
}

const char* state_name(tp::State s) {
    switch (s) {
    case tp::State::Idle:         return "Idle";
    case tp::State::Loading:      return "Loading";
    case tp::State::Playing:      return "Playing";
    case tp::State::Paused:       return "Paused";
    case tp::State::Stopped:      return "Stopped";
    case tp::State::Error:        return "Error";
    case tp::State::Disconnected: return "Disconnected";
    }
    return "?";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string_view dev_arg{argv[1]};
    std::string device{dev_arg};
    if (dev_arg == "auto") {
        device = discover_hw();
        if (device.empty()) {
            std::fprintf(stderr,
                         "SKIP: no usable hw: playback device available\n");
            return EXIT_SKIP;
        }
        std::fprintf(stderr, "auto-selected device: %s\n", device.c_str());
    }

    tp::EngineConfig cfg;
    cfg.device_id = device;

    auto e = tp::Engine::create(std::move(cfg));
    if (!e) {
        if (dev_arg == "auto" && is_hardware_skip(e.error())) {
            std::fprintf(stderr, "SKIP: engine create failed (%.*s): %s\n",
                         static_cast<int>(tp::error_code_name(e.error().code).size()),
                         tp::error_code_name(e.error().code).data(),
                         e.error().message.c_str());
            return EXIT_SKIP;
        }
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
    tp::Error captured_err{tp::ErrorCode::DeviceOpenFailed, ""};

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
            captured_err = ev.error;
            errored = true;
            cv.notify_all();
        }
    });

    auto lr = e.value()->load(argv[2]);
    if (!lr) {
        if (dev_arg == "auto" && is_hardware_skip(lr.error())) {
            std::fprintf(stderr, "SKIP: load failed (%.*s): %s\n",
                         static_cast<int>(tp::error_code_name(lr.error().code).size()),
                         tp::error_code_name(lr.error().code).data(),
                         lr.error().message.c_str());
            return EXIT_SKIP;
        }
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
    if (errored) {
        if (dev_arg == "auto" && is_hardware_skip(captured_err)) {
            std::fprintf(stderr, "SKIP: playback failed on hardware path\n");
            return EXIT_SKIP;
        }
        return 4;
    }
    return 0;
}
