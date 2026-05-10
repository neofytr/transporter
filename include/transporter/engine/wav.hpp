// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_ENGINE_WAV_HPP
#define TRANSPORTER_ENGINE_WAV_HPP

#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <vector>

namespace transporter::engine {

// In-memory WAV. Phase 1 loads the whole `data` chunk up front; the audio
// loop streams from it without further allocation. Phase 5 will replace this
// with chunked decode-into-ring.
struct WavFile {
    PcmFormat format;
    std::uint64_t total_frames;
    std::vector<std::byte> samples; // interleaved, little-endian, format.frame_bytes() per frame

    std::span<const std::byte> data() const noexcept { return samples; }
};

std::expected<WavFile, Error> load_wav(const std::filesystem::path& path);

} // namespace transporter::engine

#endif
