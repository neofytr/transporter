// SPDX-License-Identifier: GPL-3.0-or-later

#include <transporter/engine/alsa_output.hpp>
#include <transporter/engine/decoder.hpp>
#include <transporter/engine/decoder_factory.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>
#include <transporter/engine/format_match.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tp = transporter::engine;

namespace {

void print_usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <hw:CARD=X,DEV=Y> [fixtures_dir]\n"
                 "       fixtures_dir defaults to ./fixtures\n",
                 argv0);
}

constexpr std::array<std::string_view, 7> kFiles = {
    "sine_44100_16.wav",
    "sine_44100_16.aiff",
    "sine_44100_16.flac",
    "sine_44100_16.m4a",
    "sine_44100_16.mp3",
    "sine_44100_16.ogg",
    "sine_44100_16.opus",
};

int play_one(const std::string& hw, const std::filesystem::path& path) {
    std::fprintf(stderr, "--- %s\n", path.string().c_str());

    auto decoder = tp::open_decoder(path);
    if (!decoder) {
        std::fprintf(stderr, "decoder open [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(decoder.error().code).size()),
                     tp::error_code_name(decoder.error().code).data(),
                     decoder.error().message.c_str());
        return 2;
    }
    auto& dec = **decoder;
    const tp::PcmFormat fmt = dec.format();

    auto caps = tp::alsa::probe(hw);
    if (!caps) {
        std::fprintf(stderr, "device probe [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(caps.error().code).size()),
                     tp::error_code_name(caps.error().code).data(),
                     caps.error().message.c_str());
        return 3;
    }

    auto matched = tp::match(fmt, caps->view());
    if (!matched) {
        std::fprintf(stderr, "format mismatch [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(matched.error().code).size()),
                     tp::error_code_name(matched.error().code).data(),
                     matched.error().message.c_str());
        return 4;
    }

    auto out = tp::alsa::Output::open(hw, *matched);
    if (!out) {
        std::fprintf(stderr, "output open [%.*s]: %s\n",
                     static_cast<int>(tp::error_code_name(out.error().code).size()),
                     tp::error_code_name(out.error().code).data(),
                     out.error().message.c_str());
        return 5;
    }

    constexpr std::size_t MAX_FRAMES = 4096;
    const unsigned frame_bytes = matched->frame_bytes();
    std::vector<std::byte> buf(MAX_FRAMES * frame_bytes);

    std::uint64_t total = 0;
    while (true) {
        auto r = dec.read(std::span<std::byte>(buf), MAX_FRAMES);
        if (!r) {
            std::fprintf(stderr, "read [%.*s]: %s\n",
                         static_cast<int>(tp::error_code_name(r.error().code).size()),
                         tp::error_code_name(r.error().code).data(),
                         r.error().message.c_str());
            return 6;
        }
        if (*r == 0) {
            break;
        }
        const std::size_t bytes = *r * frame_bytes;
        auto wr = out->write_all(std::span<const std::byte>(buf.data(), bytes));
        if (!wr) {
            std::fprintf(stderr, "write [%.*s]: %s\n",
                         static_cast<int>(tp::error_code_name(wr.error().code).size()),
                         tp::error_code_name(wr.error().code).data(),
                         wr.error().message.c_str());
            return 7;
        }
        total += *r;
    }
    out->drain_and_close();
    std::fprintf(stderr, "ok %llu frames\n",
                 static_cast<unsigned long long>(total));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        print_usage(argv[0]);
        return 1;
    }
    const std::string hw = argv[1];
    std::filesystem::path dir = argc == 3 ? argv[2] : "fixtures";

    int rc = 0;
    for (const auto name : kFiles) {
        const auto path = dir / std::filesystem::path(std::string(name));
        rc = play_one(hw, path);
        if (rc != 0) {
            std::fprintf(stderr, "stopped at %s\n", path.string().c_str());
            return rc;
        }
    }
    std::fprintf(stderr, "all formats played.\n");
    return 0;
}
