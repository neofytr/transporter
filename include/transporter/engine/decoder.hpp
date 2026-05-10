// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_ENGINE_DECODER_HPP
#define TRANSPORTER_ENGINE_DECODER_HPP

#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace transporter::engine {

// Tags returned by the decoder. Empty string means "not present in file".
// `track_no` is a string because containers store both "3" and "3/12" forms;
// the caller decides whether to keep or trim the slash suffix. `date` is the
// file's stamp as written, year only when the file only stored a year.
struct Tags {
    std::string artist;
    std::string album;
    std::string title;
    std::string track_no;
    std::string date;
};

// Streaming PCM decoder. One instance per open file. Single-threaded; the
// engine owns it on the decode thread.
//
// `read()` writes interleaved PCM frames in the format reported by `format()`.
// Returns the number of frames written; 0 means EOF. Partial fills are
// allowed (decoder may return fewer than `max_frames` even mid-stream).
//
// `dst_frames` must be at least `max_frames * format().frame_bytes()` bytes.
// Decoders must not write past `max_frames * format().frame_bytes()`.
//
// `seek_frame` is best-effort. Implementations may snap to the nearest
// preceding seek-table entry; some lossy formats only seek to packet
// boundaries. Callers re-read after seek.
class IDecoder {
public:
    virtual ~IDecoder() = default;

    virtual PcmFormat format() const noexcept = 0;

    // 0 if unknown (rare; some streams without seek tables).
    virtual std::uint64_t total_frames() const noexcept = 0;

    // Derivable from `format()` and `total_frames()`. Convenience.
    double duration_seconds() const noexcept {
        const auto rate = format().sample_rate_hz;
        if (rate == 0) {
            return 0.0;
        }
        return static_cast<double>(total_frames()) / static_cast<double>(rate);
    }

    virtual const Tags& tags() const noexcept = 0;

    virtual std::expected<std::size_t, Error>
    read(std::span<std::byte> dst, std::size_t max_frames) = 0;

    virtual std::expected<void, Error> seek_frame(std::uint64_t frame) = 0;
};

} // namespace transporter::engine

#endif
