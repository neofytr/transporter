// SPDX-License-Identifier: GPL-3.0-or-later

#include <transporter/engine/alsa_output.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/engine/format_match.hpp>
#include <transporter/engine/wav.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace tp = transporter::engine;

namespace {

void print_usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <hw:CARD=X,DEV=Y> <file.wav>\n"
                 "       %s --inspect <file.wav>\n",
                 argv0, argv0);
}

int do_inspect(const char* path) {
    auto wav = tp::load_wav(path);
    if (!wav) {
        std::fprintf(stderr, "decode error [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(wav.error().code).size()),
                     tp::error_code_name(wav.error().code).data(),
                     wav.error().message.c_str());
        return 2;
    }
    const auto& f = wav->format;
    std::printf("rate=%u channels=%u format=%.*s frames=%llu bytes=%zu\n",
                f.sample_rate_hz, f.channels,
                static_cast<int>(tp::sample_format_name(f.sample_format).size()),
                tp::sample_format_name(f.sample_format).data(),
                static_cast<unsigned long long>(wav->total_frames), wav->samples.size());
    return 0;
}

int do_play(const char* hw_name, const char* path) {
    auto wav = tp::load_wav(path);
    if (!wav) {
        std::fprintf(stderr, "decode error [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(wav.error().code).size()),
                     tp::error_code_name(wav.error().code).data(),
                     wav.error().message.c_str());
        return 2;
    }

    auto caps = tp::alsa::probe(hw_name);
    if (!caps) {
        std::fprintf(stderr, "device probe error [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(caps.error().code).size()),
                     tp::error_code_name(caps.error().code).data(),
                     caps.error().message.c_str());
        return 3;
    }

    auto matched = tp::match(wav->format, caps->view());
    if (!matched) {
        std::fprintf(stderr, "format mismatch [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(matched.error().code).size()),
                     tp::error_code_name(matched.error().code).data(),
                     matched.error().message.c_str());
        return 4;
    }

    auto out = tp::alsa::Output::open(hw_name, *matched);
    if (!out) {
        std::fprintf(stderr, "device open error [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(out.error().code).size()),
                     tp::error_code_name(out.error().code).data(),
                     out.error().message.c_str());
        return 5;
    }

    const auto pi = out->period_info();
    std::fprintf(stderr,
                 "playing: rate=%u ch=%u fmt=%.*s period=%u frames periods=%u buffer=%u frames\n",
                 matched->sample_rate_hz, matched->channels,
                 static_cast<int>(tp::sample_format_name(matched->sample_format).size()),
                 tp::sample_format_name(matched->sample_format).data(),
                 pi.period_frames, pi.periods, pi.buffer_frames);

    auto wr = out->write_all(wav->data());
    if (!wr) {
        std::fprintf(stderr, "write error [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(wr.error().code).size()),
                     tp::error_code_name(wr.error().code).data(),
                     wr.error().message.c_str());
        return 6;
    }
    out->drain_and_close();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3) {
        const std::string_view a1{argv[1]};
        if (a1 == "--inspect") {
            return do_inspect(argv[2]);
        }
        return do_play(argv[1], argv[2]);
    }
    print_usage(argv[0]);
    return 1;
}
