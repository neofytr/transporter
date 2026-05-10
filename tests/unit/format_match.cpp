// SPDX-License-Identifier: GPL-3.0-or-later

#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/engine/format_match.hpp>
#include <transporter/engine/wav.hpp>

#include <array>
#include <cstdint>
#include <cstdio>

namespace tp = transporter::engine;

namespace {

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
    auto wav = tp::load_wav(argv[1]);
    if (!wav) {
        return fail("load_wav");
    }
    if (wav->format.sample_format != tp::SampleFormat::S24_3LE) {
        std::fprintf(stderr, "expected S24_3LE in fixture, got %.*s\n",
                     static_cast<int>(tp::sample_format_name(wav->format.sample_format).size()),
                     tp::sample_format_name(wav->format.sample_format).data());
        return fail("fixture-format");
    }

    // Synthetic device that lacks S24_3LE.
    constexpr std::array<std::uint32_t, 2> rates = {44100, 48000};
    constexpr std::array<tp::SampleFormat, 2> formats = {
        tp::SampleFormat::S16_LE,
        tp::SampleFormat::S32_LE,
    };
    const tp::DeviceCaps caps{rates, formats, 1, 2};

    auto m = tp::match(wav->format, caps);
    if (m) {
        return fail("expected refusal, got accept");
    }
    if (m.error().code != tp::ErrorCode::FormatNotSupported) {
        return fail("error code");
    }
    if (m.error().rejection != tp::FormatRejection::SampleFormatNotSupported) {
        return fail("rejection sub-reason");
    }
    std::printf("ok refused: %s\n", m.error().message.c_str());
    return 0;
}
