// SPDX-License-Identifier: GPL-3.0-or-later

#include "decoder_check.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <fixtures/sine_44100_16.wav>\n", argv[0]);
        return 2;
    }
    decoder_test::Expected e{};
    e.sample_rate = 44100;
    e.channels = 1;
    e.sample_format = tp::SampleFormat::S16_LE;
    e.lossless = true;
    e.expected_total_frames = 88200;
    e.check_tags = false;
    return decoder_test::run_check(argv[1], e);
}
