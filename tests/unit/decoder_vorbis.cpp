// SPDX-License-Identifier: GPL-3.0-or-later

#include "decoder_check.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <fixtures/sine_44100_16.ogg>\n", argv[0]);
        return 2;
    }
    decoder_test::Expected e{};
    e.sample_rate = 44100;
    e.channels = 1;
    e.sample_format = tp::SampleFormat::FLOAT_LE;
    e.lossless = false;
    e.expected_total_frames = 88200;
    e.total_frames_slack = 2048;
    e.check_tags = true;
    e.artist = "transporter test";
    e.album = "phase2 fixtures";
    e.title = "sine 440Hz";
    e.track_no = "1";
    e.date = "2024";
    return decoder_test::run_check(argv[1], e);
}
