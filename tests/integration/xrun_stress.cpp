// SPDX-License-Identifier: GPL-3.0-or-later
//
// xrun stress harness. Drives long playback through the production Engine
// against a real `hw:` device and prints the running xrun_count from
// `Engine::pipeline_snapshot()` once per second. Designed to run alongside
// `stress-ng` from a sibling shell so the impact of competing CPU + memory
// load on the audio path is measurable.
//
// argv:
//   <hw:device>                 e.g. hw:CARD=DAC,DEV=0
//   <duration_seconds>          total wall-clock budget; default 60
//   [<file_to_loop>]            default fixtures/sine_44100_16.flac
//
// Exit codes:
//   0 — clean run (engine ran the full duration without an unrecoverable
//       error). The caller decides what xrun_total is acceptable.
//   1 — engine errored or refused to load.
//   2 — argv usage problem.

#include <transporter/engine/device.hpp>
#include <transporter/engine/engine.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/engine/telemetry.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace tp = transporter::engine;

namespace {

constexpr int EXIT_SKIP = 77;

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

const char* rt_mode_name(tp::rt::Mode m) {
    return m == tp::rt::Mode::Fifo ? "FIFO" : "OTHER";
}

void print_usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <hw:device|auto> [<duration_seconds>=60] [<file>]\n"
                 "  Run the engine against the given hw: device for the\n"
                 "  given duration, looping <file> repeatedly. Prints xrun\n"
                 "  count once per second on stderr.\n"
                 "  Companion command (in another shell):\n"
                 "    stress-ng --cpu $(($(nproc) - 1)) --vm 2 --vm-bytes 256M --io 2 --timeout 5m\n",
                 argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) {
        print_usage(argv[0]);
        return 2;
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
    const int duration_s = (argc >= 3) ? std::atoi(argv[2]) : 60;
    const std::filesystem::path file =
        (argc >= 4) ? std::filesystem::path{argv[3]}
                    : std::filesystem::path{"fixtures/sine_44100_16.flac"};

    if (duration_s <= 0) {
        std::fprintf(stderr, "duration must be > 0\n");
        return 2;
    }
    if (!std::filesystem::exists(file)) {
        std::fprintf(stderr, "file not found: %s\n", file.c_str());
        return 1;
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
        return 1;
    }

    std::mutex mtx;
    std::condition_variable cv;
    bool ended = false;
    bool errored = false;
    tp::Error err{tp::ErrorCode::DeviceOpenFailed, ""};

    e.value()->set_event_callback([&](const tp::Event& ev) {
        if (ev.kind == tp::Event::Kind::StateChanged) {
            std::fprintf(stderr, "state -> %s\n", state_name(ev.state));
        } else if (ev.kind == tp::Event::Kind::TrackEnded) {
            std::lock_guard lk(mtx);
            ended = true;
            cv.notify_all();
        } else if (ev.kind == tp::Event::Kind::ErrorOccurred) {
            std::lock_guard lk(mtx);
            err = ev.error;
            errored = true;
            cv.notify_all();
        }
    });

    tp::Error last_load_err{tp::ErrorCode::DeviceOpenFailed, ""};
    auto load_or_die = [&](const std::filesystem::path& f) -> bool {
        auto lr = e.value()->load(f);
        if (!lr) {
            last_load_err = lr.error();
            std::fprintf(stderr, "load [%.*s]: %s\n",
                         static_cast<int>(tp::error_code_name(lr.error().code).size()),
                         tp::error_code_name(lr.error().code).data(),
                         lr.error().message.c_str());
            return false;
        }
        return true;
    };

    if (!load_or_die(file)) {
        if (dev_arg == "auto" && is_hardware_skip(last_load_err)) {
            std::fprintf(stderr, "SKIP: load failed on hardware path\n");
            return EXIT_SKIP;
        }
        return 1;
    }

    const auto t_start = std::chrono::steady_clock::now();
    const auto t_deadline = t_start + std::chrono::seconds(duration_s);

    auto next_print = t_start + std::chrono::seconds(1);
    std::uint32_t last_xrun = 0;

    while (std::chrono::steady_clock::now() < t_deadline) {
        std::unique_lock lk(mtx);
        cv.wait_until(lk, next_print, [&] { return ended || errored; });
        if (errored) {
            std::fprintf(stderr, "engine errored [%.*s]: %s\n",
                         static_cast<int>(tp::error_code_name(err.code).size()),
                         tp::error_code_name(err.code).data(),
                         err.message.c_str());
            if (dev_arg == "auto" && is_hardware_skip(err)) {
                std::fprintf(stderr,
                             "SKIP: playback failed on hardware path\n");
                return EXIT_SKIP;
            }
            return 1;
        }
        if (ended) {
            ended = false;
            lk.unlock();
            // Re-arm by reloading the same file. load() also kicks state
            // back to Loading -> Playing.
            if (!load_or_die(file)) {
                if (dev_arg == "auto" && is_hardware_skip(last_load_err)) {
                    std::fprintf(stderr,
                                 "SKIP: reload failed on hardware path\n");
                    return EXIT_SKIP;
                }
                return 1;
            }
            lk.lock();
        }
        if (std::chrono::steady_clock::now() >= next_print) {
            const auto snap = e.value()->pipeline_snapshot();
            const auto elapsed_s =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - t_start)
                    .count();
            const std::uint32_t x = snap.output.xrun_count;
            std::fprintf(stderr,
                         "t=%lld xrun=%u\n",
                         static_cast<long long>(elapsed_s), x);
            last_xrun = x;
            next_print += std::chrono::seconds(1);
        }
    }

    const auto snap = e.value()->pipeline_snapshot();
    std::fprintf(stderr,
                 "FINAL xrun_total=%u rt_mode=%s frames_written=%llu\n",
                 snap.output.xrun_count,
                 rt_mode_name(snap.realtime.status.mode),
                 static_cast<unsigned long long>(snap.output.frames_written));
    (void)last_xrun;
    return 0;
}
