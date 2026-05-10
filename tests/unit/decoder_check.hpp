// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_TESTS_DECODER_CHECK_HPP
#define TRANSPORTER_TESTS_DECODER_CHECK_HPP

#include <transporter/engine/decoder.hpp>
#include <transporter/engine/decoder_factory.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace tp = transporter::engine;

namespace decoder_test {

inline int fail(const char* where, std::string_view detail = {}) {
    if (detail.empty()) {
        std::fprintf(stderr, "FAIL [%s]\n", where);
    } else {
        std::fprintf(stderr, "FAIL [%s]: %.*s\n", where,
                     static_cast<int>(detail.size()), detail.data());
    }
    return 1;
}

struct Expected {
    std::uint32_t sample_rate;
    std::uint16_t channels;
    tp::SampleFormat sample_format;
    bool lossless; // tight equality on total_frames
    std::uint64_t expected_total_frames;
    std::uint64_t total_frames_slack;
    bool check_tags;
    const char* artist;
    const char* album;
    const char* title;
    const char* track_no;
    const char* date;
};

inline int run_check(const char* path, const Expected& exp) {
    auto open = tp::open_decoder(path);
    if (!open) {
        return fail("open_decoder", open.error().message);
    }
    auto& dec = **open;
    const tp::PcmFormat fmt = dec.format();

    if (fmt.sample_rate_hz != exp.sample_rate) {
        return fail("sample_rate", std::to_string(fmt.sample_rate_hz));
    }
    if (fmt.channels != exp.channels) {
        return fail("channels", std::to_string(fmt.channels));
    }
    if (fmt.sample_format != exp.sample_format) {
        return fail("sample_format",
                    tp::sample_format_name(fmt.sample_format));
    }
    if (dec.total_frames() == 0) {
        return fail("total_frames", "0");
    }
    if (exp.lossless) {
        if (dec.total_frames() != exp.expected_total_frames) {
            return fail("total_frames!=expected",
                        std::to_string(dec.total_frames()));
        }
    } else {
        const auto t = dec.total_frames();
        const auto e = exp.expected_total_frames;
        const auto slack = exp.total_frames_slack;
        const std::uint64_t lo = e > slack ? e - slack : 0;
        const std::uint64_t hi = e + slack;
        if (t < lo || t > hi) {
            return fail("total_frames out of slack window",
                        std::to_string(t));
        }
    }

    // Drain. Buffer one period (~16 KB).
    constexpr std::size_t MAX_FRAMES = 4096;
    const unsigned frame_bytes = fmt.frame_bytes();
    std::vector<std::byte> buf(MAX_FRAMES * frame_bytes);
    std::uint64_t total_read = 0;
    while (true) {
        auto r = dec.read(std::span<std::byte>(buf), MAX_FRAMES);
        if (!r) {
            return fail("read", r.error().message);
        }
        if (*r == 0) {
            break;
        }
        total_read += *r;
    }

    if (exp.lossless) {
        if (total_read != exp.expected_total_frames) {
            return fail("read sum != expected",
                        std::to_string(total_read));
        }
    } else {
        const std::uint64_t e = exp.expected_total_frames;
        const std::uint64_t slack = exp.total_frames_slack;
        const std::uint64_t lo = e > slack ? e - slack : 0;
        const std::uint64_t hi = e + slack;
        if (total_read < lo || total_read > hi) {
            return fail("read sum out of slack window",
                        std::to_string(total_read));
        }
    }

    // Confirm a second read after EOF stays at 0.
    {
        auto r = dec.read(std::span<std::byte>(buf), MAX_FRAMES);
        if (!r) {
            return fail("post-EOF read", r.error().message);
        }
        if (*r != 0) {
            return fail("post-EOF read", std::to_string(*r));
        }
    }

    if (exp.check_tags) {
        const auto& t = dec.tags();
        auto eq = [](const std::string& got, const char* want, const char* where) {
            if (!want) {
                return 0;
            }
            if (got != want) {
                std::fprintf(stderr,
                             "FAIL [tag %s]: got '%s' want '%s'\n",
                             where, got.c_str(), want);
                return 1;
            }
            return 0;
        };
        int rc = 0;
        rc |= eq(t.artist, exp.artist, "artist");
        rc |= eq(t.album, exp.album, "album");
        rc |= eq(t.title, exp.title, "title");
        rc |= eq(t.track_no, exp.track_no, "track");
        rc |= eq(t.date, exp.date, "date");
        if (rc != 0) {
            return rc;
        }
    }

    std::printf(
        "ok rate=%u ch=%u fmt=%.*s frames=%llu duration=%.3fs read=%llu\n",
        fmt.sample_rate_hz, fmt.channels,
        static_cast<int>(tp::sample_format_name(fmt.sample_format).size()),
        tp::sample_format_name(fmt.sample_format).data(),
        static_cast<unsigned long long>(dec.total_frames()),
        dec.duration_seconds(),
        static_cast<unsigned long long>(total_read));
    return 0;
}

} // namespace decoder_test

#endif
