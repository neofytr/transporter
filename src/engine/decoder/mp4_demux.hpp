// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_ENGINE_DECODER_MP4_DEMUX_HPP
#define TRANSPORTER_ENGINE_DECODER_MP4_DEMUX_HPP

#include <transporter/engine/decoder.hpp>
#include <transporter/engine/error.hpp>

#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <memory>
#include <vector>

// Minimal MP4 atom walker for ALAC-only `.m4a`/`.mp4` files. Limitations:
// - Single audio track. Refuses files with multiple tracks for sanity.
// - Refuses fragmented MP4 (`moof`/`mvex`).
// - 64-bit sample tables only via `co64` (also handles `stco`).
// - Does not validate edit lists (`elst`); we read raw sample order.
// - Reads `ilst` tags out of `moov.udta.meta.ilst`.

namespace transporter::engine::mp4 {

struct Sample {
    std::uint64_t offset; // absolute file offset
    std::uint32_t size;   // bytes
};

struct ParseResult {
    std::vector<std::byte> alac_magic_cookie; // ALAC config blob
    std::vector<Sample> samples;
    std::uint32_t sample_rate_hz = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
    std::uint32_t timescale = 0;
    std::uint64_t duration_in_timescale = 0;
    std::uint32_t default_samples_per_packet = 0; // STSZ default if uniform

    // Tags from moov.udta.meta.ilst (the "©nam" / "©ART" / "©alb" / "©day" / "trkn" set).
    Tags tags{};
};

// Parses the file. Caller passes a stdio file handle positioned anywhere; we
// fseek as needed. Ownership stays with the caller.
std::expected<ParseResult, Error> parse(std::FILE* fp);

} // namespace transporter::engine::mp4

#endif
