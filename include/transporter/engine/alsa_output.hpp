// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_ENGINE_ALSA_OUTPUT_HPP
#define TRANSPORTER_ENGINE_ALSA_OUTPUT_HPP

#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
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

// Open-time options. target_period_ms scales period frame count per active
// rate (12 ms default at 44.1 k => 528 frames; 192 k => 2304 frames). An
// xrun_observer is invoked from write_all on each recover; it must not
// allocate, lock, or block — the audio thread is calling it.
struct OpenOptions {
    unsigned target_period_ms = 12;
    unsigned periods_target = 4;
    std::function<void(int errno_neg)> xrun_observer;
};

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
    static std::expected<Output, Error>
    open(const std::string& hw_name, const PcmFormat& fmt, const OpenOptions& opts);

    PeriodInfo period_info() const noexcept;
    const PcmFormat& format() const noexcept;

    // writei loop with -EPIPE / -ESTRPIPE recovery via snd_pcm_recover.
    // Synchronous for Phase 1; consumes all input frames or errors. Returns
    // the number of frames written on success; on error, the count is whatever
    // got through before the failure.
    std::expected<std::size_t, Error>
    write_all(std::span<const std::byte> interleaved_frames);

    // Drain remaining frames, then close. Idempotent.
    void drain_and_close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Output(std::unique_ptr<Impl>);
};

} // namespace transporter::engine::alsa

#endif
