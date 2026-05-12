// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bit-perfect verification harness. Plays a reference WAV through ALSA's
// `Loopback` virtual card via the production Engine, captures the output via
// the linked capture endpoint, and byte-compares the captured payload
// against the source WAV's `data` chunk.
//
// Exit codes:
//   0 — captured bytes match source byte-for-byte
//   1 — mismatch (digital path mutated bytes; this is a bug)
//   2 — setup failure (snd-aloop not loaded, fixture missing, engine open
//       refused, etc.). Diagnostic message identifies the cause.

#include "../support/byte_compare.hpp"
#include "../support/loopback_capture.hpp"

#include <transporter/engine/engine.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/engine/wav.hpp>

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <utility>

namespace tp = transporter::engine;
namespace tt = transporter::testing;

namespace {

constexpr int EXIT_MATCH = 0;
constexpr int EXIT_MISMATCH = 1;
constexpr int EXIT_SETUP = 2;
constexpr int EXIT_SKIP = 77;

void print_setup_help() {
    std::fprintf(stderr,
                 "snd-aloop not loaded. Load the kernel module:\n"
                 "    sudo modprobe snd-aloop\n"
                 "    aplay -L | grep -i loopback   # confirm the card appears\n"
                 "See README.md, section \"Bit-perfect verification\".\n");
}

// Resolves the ALSA card index for a card whose short or long name contains
// `needle` (case-insensitive). Returns -1 if no card matches.
int find_card_by_name(const std::string& needle) {
    int card = -1;
    while (true) {
        if (snd_card_next(&card) < 0 || card < 0) {
            break;
        }
        char* longname = nullptr;
        char* shortname = nullptr;
        const int ln_rc = snd_card_get_longname(card, &longname);
        const int sn_rc = snd_card_get_name(card, &shortname);
        std::string ln = (ln_rc == 0 && longname) ? longname : "";
        std::string sn = (sn_rc == 0 && shortname) ? shortname : "";
        if (longname) std::free(longname);
        if (shortname) std::free(shortname);

        auto lower = [](std::string s) {
            for (auto& c : s) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return s;
        };
        const std::string ln_l = lower(ln);
        const std::string sn_l = lower(sn);
        const std::string nl = lower(needle);
        if (ln_l.find(nl) != std::string::npos ||
            sn_l.find(nl) != std::string::npos) {
            return card;
        }
    }
    return -1;
}

void print_usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <loopback_card_id|auto> <fixture.wav>\n"
                 "  loopback_card_id may be a numeric index or a card name\n"
                 "  substring (case-insensitive). \"Loopback\" is the\n"
                 "  conventional name created by `modprobe snd-aloop`.\n"
                 "  \"auto\" searches for any card named \"Loopback\".\n",
                 argv0);
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
        return EXIT_SETUP;
    }

    std::string card_arg = argv[1];
    const std::filesystem::path fixture = argv[2];

    if (!std::filesystem::exists(fixture)) {
        std::fprintf(stderr, "fixture not found: %s\n", fixture.c_str());
        return EXIT_SETUP;
    }

    const bool auto_mode = (card_arg == "auto");
    if (auto_mode) {
        card_arg = "Loopback";
    }

    int card_index = -1;
    if (!card_arg.empty() &&
        std::all_of(card_arg.begin(), card_arg.end(),
                    [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        card_index = std::atoi(card_arg.c_str());
    } else {
        card_index = find_card_by_name(card_arg);
    }
    if (card_index < 0) {
        if (auto_mode) {
            std::fprintf(stderr,
                         "SKIP: snd-aloop not loaded "
                         "(no card named \"Loopback\" found)\n");
            return EXIT_SKIP;
        }
        std::fprintf(stderr,
                     "no ALSA card matches \"%s\".\n", card_arg.c_str());
        print_setup_help();
        return EXIT_SETUP;
    }

    char* card_name_raw = nullptr;
    if (snd_card_get_name(card_index, &card_name_raw) < 0 || !card_name_raw) {
        std::fprintf(stderr, "snd_card_get_name(%d) failed\n", card_index);
        return EXIT_SETUP;
    }
    const std::string card_name = card_name_raw;
    std::free(card_name_raw);

    // snd-aloop pairs DEV=0 (playback) <-> DEV=1 (capture) and DEV=1 (playback) <->
    // DEV=0 (capture). We push to DEV=0 and read from DEV=1.
    const std::string play_dev = "hw:CARD=" + card_name + ",DEV=0";
    const std::string cap_dev = "hw:CARD=" + card_name + ",DEV=1";

    auto wav = tp::load_wav(fixture);
    if (!wav) {
        std::fprintf(stderr, "fixture decode failed [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(wav.error().code).size()),
                     tp::error_code_name(wav.error().code).data(),
                     wav.error().message.c_str());
        return EXIT_SETUP;
    }

    std::fprintf(stderr,
                 "fixture: rate=%u ch=%u fmt=%.*s frames=%llu data_bytes=%zu\n",
                 wav->format.sample_rate_hz, wav->format.channels,
                 static_cast<int>(tp::sample_format_name(wav->format.sample_format).size()),
                 tp::sample_format_name(wav->format.sample_format).data(),
                 static_cast<unsigned long long>(wav->total_frames),
                 wav->samples.size());
    std::fprintf(stderr, "loopback card: %s (idx=%d)\n",
                 card_name.c_str(), card_index);
    std::fprintf(stderr, "  play -> %s\n  cap  <- %s\n",
                 play_dev.c_str(), cap_dev.c_str());

    // Capture side first: snd-aloop links a playback to a capture endpoint
    // and the capture won't see frames written before its own open in some
    // configurations. Open capture, prepare, start, then begin playback.
    tt::LoopbackCaptureConfig cap_cfg;
    cap_cfg.hw_name = cap_dev;
    cap_cfg.format = wav->format;
    cap_cfg.silence_window_ms = std::chrono::milliseconds{50};
    cap_cfg.period_frames = 1024;

    tt::LoopbackCapture cap;
    auto cap_rc = cap.start(cap_cfg);
    if (!cap_rc) {
        std::fprintf(stderr, "capture open failed [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(cap_rc.error().code).size()),
                     tp::error_code_name(cap_rc.error().code).data(),
                     cap_rc.error().message.c_str());
        return EXIT_SETUP;
    }

    // Production engine path. EngineConfig::device_id targets the loopback's
    // playback endpoint exactly as if it were a real DAC.
    tp::EngineConfig cfg;
    cfg.device_id = play_dev;
    auto e = tp::Engine::create(std::move(cfg));
    if (!e) {
        std::fprintf(stderr, "engine create failed [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(e.error().code).size()),
                     tp::error_code_name(e.error().code).data(),
                     e.error().message.c_str());
        return EXIT_SETUP;
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
            std::fprintf(stderr,
                         "loaded: rate=%u ch=%u total=%llu\n",
                         ev.format.sample_rate_hz, ev.format.channels,
                         static_cast<unsigned long long>(ev.total_frames));
        } else if (ev.kind == tp::Event::Kind::TrackEnded) {
            std::lock_guard lk(mtx);
            ended = true;
            cv.notify_all();
        } else if (ev.kind == tp::Event::Kind::ErrorOccurred) {
            std::lock_guard lk(mtx);
            captured_err = ev.error;
            errored = true;
            cv.notify_all();
        }
    });

    auto lr = e.value()->load(fixture);
    if (!lr) {
        std::fprintf(stderr, "load failed [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(lr.error().code).size()),
                     tp::error_code_name(lr.error().code).data(),
                     lr.error().message.c_str());
        cap.stop();
        cap.join();
        return EXIT_SETUP;
    }

    // Bound the wait to 60 s. Fixtures are 2 s; the engine drives the
    // playback through TrackEnded which is emitted post-drain.
    {
        std::unique_lock lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(60), [&] { return ended || errored; });
    }
    if (errored) {
        std::fprintf(stderr, "engine errored [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(captured_err.code).size()),
                     tp::error_code_name(captured_err.code).data(),
                     captured_err.message.c_str());
        cap.stop();
        cap.join();
        return EXIT_SETUP;
    }
    if (!ended) {
        std::fprintf(stderr, "timed out waiting for TrackEnded\n");
        cap.stop();
        cap.join();
        return EXIT_SETUP;
    }

    // Tear down the engine before stopping capture so the playback side has
    // fully drained into the loopback's ring before the capture pulls the
    // tail.
    e.value().reset();

    cap.stop();
    auto captured = cap.join();

    std::fprintf(stderr, "captured %zu bytes (source has %zu data bytes)\n",
                 captured.size(), wav->samples.size());

    // The capture is permitted to be longer than the source: snd-aloop
    // streams zeros before playback fills the ring and after it drains.
    // Locate the start of non-zero capture and align from there.
    std::size_t start = 0;
    while (start < captured.size() && captured[start] == std::byte{0}) {
        ++start;
    }
    if (start == captured.size()) {
        std::fprintf(stderr,
                     "capture is entirely silent; loopback link inactive\n");
        return EXIT_MISMATCH;
    }
    if (captured.size() - start < wav->samples.size()) {
        std::fprintf(stderr,
                     "capture short by %zu bytes after silence trim\n",
                     wav->samples.size() - (captured.size() - start));
        return EXIT_MISMATCH;
    }

    std::span<const std::byte> ref{wav->samples.data(), wav->samples.size()};
    std::span<const std::byte> cap_span{captured.data() + start,
                                        captured.size() - start};

    auto cmp = tt::compare_bytes(ref, cap_span);
    if (cmp.match) {
        std::fprintf(stderr,
                     "MATCH: %zu bytes byte-identical (source -> loopback -> capture)\n",
                     cmp.reference_size);
        return EXIT_MATCH;
    }

    std::fprintf(stderr,
                 "MISMATCH: %zu of %zu bytes differ; first at offset %zu\n"
                 "  reference[%zu] = 0x%02x\n"
                 "  capture  [%zu] = 0x%02x\n",
                 cmp.mismatch_count, cmp.reference_size, cmp.mismatch_offset,
                 cmp.mismatch_offset, cmp.reference_byte,
                 cmp.mismatch_offset, cmp.capture_byte);
    return EXIT_MISMATCH;
}
