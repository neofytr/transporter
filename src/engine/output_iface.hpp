// SPDX-License-Identifier: GPL-3.0-or-later
//
// Engine-internal output abstraction. Real ALSA output adapts to this; unit
// tests substitute a mock. NOT a public engine header — keep behind src/.

#ifndef TRANSPORTER_ENGINE_OUTPUT_IFACE_HPP
#define TRANSPORTER_ENGINE_OUTPUT_IFACE_HPP

#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace transporter::engine::detail {

// Plain caps snapshot to keep IDevice independent of the alsa header. Spans
// returned by view() point into stable storage owned by the snapshot.
struct CapsView {
    std::vector<std::uint32_t> rates;
    std::vector<SampleFormat> formats;
    std::uint16_t min_channels{0};
    std::uint16_t max_channels{0};

    DeviceCaps view() const noexcept {
        return DeviceCaps{rates, formats, min_channels, max_channels};
    }
};

// IOutput models the engine's view of an opened device. The engine owns the
// IOutput; on rate transitions the engine destroys it and constructs a new
// one. write_all is the audio-thread hot path; it must not allocate.
//
// write_all returns the number of frames successfully written (may be
// fewer than requested only on the rare partial-on-error path; on success
// it is the requested frame count).
class IOutput {
public:
    virtual ~IOutput() = default;

    virtual const PcmFormat& format() const noexcept = 0;
    virtual std::expected<std::size_t, Error>
    write_all(std::span<const std::byte> interleaved) = 0;
    virtual void drop_and_close() noexcept = 0;

    // Period / buffer info; used by telemetry. Default implementation
    // returns zeroes (acceptable for mocks).
    struct PeriodInfo {
        std::uint32_t period_frames = 0;
        std::uint32_t periods = 0;
        std::uint32_t buffer_frames = 0;
    };
    virtual PeriodInfo period_info() const noexcept { return {}; }
};

// Open options as the engine passes them across the IDevice seam. xrun_cb
// fires on each ALSA recover; must not allocate / lock / block.
struct OpenOpts {
    unsigned target_period_ms = 12;
    unsigned periods_target = 4;
    std::function<void(int)> xrun_cb;
};

// IDevice abstracts probe + open. Real impl wraps alsa::probe and
// alsa::Output::open; mock impl drives capabilities and write outcomes from
// tests.
class IDevice {
public:
    virtual ~IDevice() = default;

    virtual std::expected<CapsView, Error> probe_caps() = 0;
    virtual std::expected<std::unique_ptr<IOutput>, Error>
    open(const PcmFormat& fmt) = 0;
    virtual std::expected<std::unique_ptr<IOutput>, Error>
    open(const PcmFormat& fmt, const OpenOpts& opts) {
        // Default delegates to plain open(); subclasses override to honour
        // period_ms / xrun_cb.
        (void)opts;
        return open(fmt);
    }
};

} // namespace transporter::engine::detail

#endif
