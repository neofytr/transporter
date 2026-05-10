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
class IOutput {
public:
    virtual ~IOutput() = default;

    virtual const PcmFormat& format() const noexcept = 0;
    virtual std::expected<void, Error>
    write_all(std::span<const std::byte> interleaved) = 0;
    virtual void drop_and_close() noexcept = 0;
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
};

} // namespace transporter::engine::detail

#endif
