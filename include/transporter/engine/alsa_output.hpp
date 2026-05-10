// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_ENGINE_ALSA_OUTPUT_HPP
#define TRANSPORTER_ENGINE_ALSA_OUTPUT_HPP

#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace transporter::engine::alsa {

// Probe the device for the format/rate/channel knobs we care about. Always
// uses the exact `_test_*` calls; never `_near`. Returns one capability set
// owned by the caller; caps' spans point into the returned vectors via the
// stable Storage pair.
struct DeviceCapsStorage {
    std::vector<std::uint32_t> rates;
    std::vector<SampleFormat> formats;
    std::uint16_t min_channels;
    std::uint16_t max_channels;

    DeviceCaps view() const noexcept {
        return DeviceCaps{rates, formats, min_channels, max_channels};
    }
};

std::expected<DeviceCapsStorage, Error> probe(const std::string& hw_name);

// Opaque handle: holds the snd_pcm_t plus its negotiated parameters.
class Output {
public:
    struct PeriodInfo {
        std::uint32_t period_frames;
        std::uint32_t periods;
        std::uint32_t buffer_frames;
    };

    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    Output(Output&&) noexcept;
    Output& operator=(Output&&) noexcept;
    ~Output();

    static std::expected<Output, Error> open(const std::string& hw_name, const PcmFormat& fmt);

    PeriodInfo period_info() const noexcept;
    const PcmFormat& format() const noexcept;

    // writei loop with -EPIPE / -ESTRPIPE recovery via snd_pcm_recover.
    // Synchronous for Phase 1; consumes all input frames or errors.
    std::expected<void, Error> write_all(std::span<const std::byte> interleaved_frames);

    // Drain remaining frames, then close. Idempotent.
    void drain_and_close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Output(std::unique_ptr<Impl>);
};

} // namespace transporter::engine::alsa

#endif
