// SPDX-License-Identifier: GPL-3.0-or-later

#include <transporter/engine/format.hpp>
#include <transporter/engine/wav.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace tp = transporter::engine;

namespace {

int fail(const char* where, const std::string& detail) {
    std::fprintf(stderr, "FAIL [%s]: %s\n", where, detail.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <file.wav>\n", argv[0]);
        return 2;
    }
    auto wav = tp::load_wav(argv[1]);
    if (!wav) {
        return fail("load_wav", wav.error().message);
    }
    const auto& f = wav->format;
    if (f.sample_rate_hz != 44100u) {
        return fail("rate", std::to_string(f.sample_rate_hz));
    }
    if (f.channels != 1u) {
        return fail("channels", std::to_string(f.channels));
    }
    if (f.sample_format != tp::SampleFormat::S16_LE) {
        return fail("format", std::string(tp::sample_format_name(f.sample_format)));
    }
    if (wav->total_frames == 0u) {
        return fail("frames", "0");
    }
    std::printf("ok rate=%u ch=%u fmt=S16_LE frames=%llu\n", f.sample_rate_hz,
                f.channels, static_cast<unsigned long long>(wav->total_frames));
    return 0;
}
