// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_ENGINE_FORMAT_HPP
#define TRANSPORTER_ENGINE_FORMAT_HPP

#include <cstdint>
#include <span>
#include <string_view>

namespace transporter::engine {

// PCM sample formats, all little-endian on disk and on the wire to libasound.
// S24_LE is 24-bit data in 32-bit container (low 3 bytes used). S24_3LE is
// tightly packed 3 bytes per sample. Different DACs accept different packing.
enum class SampleFormat : std::uint8_t {
    S16_LE,
    S24_LE,
    S24_3LE,
    S32_LE,
    FLOAT_LE,
};

constexpr std::string_view sample_format_name(SampleFormat f) noexcept {
    switch (f) {
    case SampleFormat::S16_LE:
        return "S16_LE";
    case SampleFormat::S24_LE:
        return "S24_LE";
    case SampleFormat::S24_3LE:
        return "S24_3LE";
    case SampleFormat::S32_LE:
        return "S32_LE";
    case SampleFormat::FLOAT_LE:
        return "FLOAT_LE";
    }
    return "?";
}

constexpr unsigned sample_format_bytes_per_sample(SampleFormat f) noexcept {
    switch (f) {
    case SampleFormat::S16_LE:
        return 2;
    case SampleFormat::S24_3LE:
        return 3;
    case SampleFormat::S24_LE:
    case SampleFormat::S32_LE:
    case SampleFormat::FLOAT_LE:
        return 4;
    }
    return 0;
}

struct PcmFormat {
    std::uint32_t sample_rate_hz;
    std::uint16_t channels;
    SampleFormat sample_format;

    constexpr unsigned frame_bytes() const noexcept {
        return sample_format_bytes_per_sample(sample_format) * channels;
    }
};

// Snapshot of a device's accepted parameters. Built by an ALSA capability
// probe. No "near" matches: a rate or format is either present or it isn't.
struct DeviceCaps {
    std::span<const std::uint32_t> rates;
    std::span<const SampleFormat> formats;
    std::uint16_t min_channels;
    std::uint16_t max_channels;
};

} // namespace transporter::engine

#endif
