// SPDX-License-Identifier: GPL-3.0-or-later

#include "decoder_check.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <fixtures/sine_44100_16.opus>\n", argv[0]);
        return 2;
    }
    decoder_test::Expected e{};
    // Opus always reports 48 kHz output regardless of source rate.
    e.sample_rate = 48000;
    e.channels = 1;
    e.sample_format = tp::SampleFormat::FLOAT_LE;
    e.lossless = false;
    // 2.0 s at 48 kHz = 96000 frames; allow encoder framing slack.
    e.expected_total_frames = 96000;
    e.total_frames_slack = 2048;
    e.check_tags = true;
    e.artist = "transporter test";
    e.album = "phase2 fixtures";
    e.title = "sine 440Hz";
    e.track_no = "1";
    e.date = "2024";
    return decoder_test::run_check(argv[1], e);
}
